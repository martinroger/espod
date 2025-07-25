#include "esPod.h"

//-----------------------------------------------------------------------
//|                      Cardinal tasks and Timers                      |
//-----------------------------------------------------------------------
#pragma region Tasks
/// @brief RX Task, sifts through the incoming serial data and compiles packets that pass the checksum and passes them to the processing Queue _cmdQueue. Also handles timeouts and can trigger state resets.
/// @param pvParameters Unused
void esPod::_rxTask(void *pvParameters)
{
    esPod *esPodInstance = static_cast<esPod *>(pvParameters);

    byte prevByte = 0x00;
    byte incByte = 0x00;
    byte buf[MAX_PACKET_SIZE] = {0x00};
    uint32_t expLength = 0;
    uint32_t cursor = 0;

    unsigned long lastByteRX = millis();   // Last time a byte was RXed in a packet
    unsigned long lastActivity = millis(); // Last time any RX activity was detected

    aapCommand cmd;

#ifdef STACK_HIGH_WATERMARK_LOG
    UBaseType_t uxHighWaterMark;
    UBaseType_t minHightWaterMark = RX_TASK_STACK_SIZE;
#endif

    while (true)
    {
// Stack high watermark logging
#ifdef STACK_HIGH_WATERMARK_LOG
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
        if (uxHighWaterMark < minHightWaterMark)
        {
            minHightWaterMark = uxHighWaterMark;
            ESP_LOGI("HWM", "RX Task High Watermark: %d, used stack: %d", minHightWaterMark, RX_TASK_STACK_SIZE - minHightWaterMark);
        }
#endif

        // If the esPod is disabled, flush the RX buffer and wait for 2*RX_TASK_INTERVAL_MS before checking again
        if (esPodInstance->disabled)
        {
            while (esPodInstance->_targetSerial.available())
            {
                esPodInstance->_targetSerial.read();
            }
            vTaskDelay(pdMS_TO_TICKS(2 * RX_TASK_INTERVAL_MS));
            continue;
        }
        else // esPod is enabled, process away !
        {
            // Use of while instead of if()
            while (esPodInstance->_targetSerial.available())
            {
                // Timestamping the last activity on RX
                lastActivity = millis();
                incByte = esPodInstance->_targetSerial.read();
                // If we are not in the middle of a RX, and we receive a 0xFF 0x55, start sequence, reset expected length and position cursor
                if (prevByte == 0xFF && incByte == 0x55 && !esPodInstance->_rxIncomplete)
                {
                    lastByteRX = millis();
                    esPodInstance->_rxIncomplete = true;
                    expLength = 0;
                    cursor = 0;
                }
                else if (esPodInstance->_rxIncomplete)
                {
                    // Timestamping the last byte received
                    lastByteRX = millis();
                    // Expected length has not been received yet
                    if (expLength == 0 && cursor == 0)
                    {
                        expLength = incByte; // First byte after 0xFF 0x55
                        if (expLength > MAX_PACKET_SIZE)
                        {
                            ESP_LOGW(__func__, "Expected length is too long, discarding packet");
                            esPodInstance->_rxIncomplete = false;
                            // TODO: Send a NACK to the Accessory
                        }
                        else if (expLength == 0)
                        {
                            ESP_LOGW(__func__, "Expected length is 0, discarding packet");
                            esPodInstance->_rxIncomplete = false;
                            // TODO: Send a NACK to the Accessory
                        }
                    }
                    else // Length is already received
                    {
                        buf[cursor++] = incByte;
                        if (cursor == expLength + 1)
                        {
                            // We have received the expected length + checksum
                            esPodInstance->_rxIncomplete = false;
                            // Check the checksum
                            byte calcChecksum = esPod::_checksum(buf, expLength);
                            if (calcChecksum == incByte)
                            {
                                // Checksum is correct, send the packet to the processing queue
                                // Allocate memory for the payload so it doesn't become out of scope
                                cmd.payload = new byte[expLength];
                                cmd.length = expLength;
                                memcpy(cmd.payload, buf, expLength);
                                if (xQueueSend(esPodInstance->_cmdQueue, &cmd, pdMS_TO_TICKS(5)) == pdTRUE)
                                {
                                    ESP_LOGD(__func__, "Packet received and sent to processing queue");
                                }
                                else
                                {
                                    ESP_LOGW(__func__, "Packet received but could not be sent to processing queue. Discarding");
                                    delete[] cmd.payload;
                                    cmd.payload = nullptr;
                                    cmd.length = 0;
                                }
                            }
                            else // Checksum mismatch
                            {
                                ESP_LOGW(__func__, "Checksum mismatch, discarding packet");
                                // TODO: Send a NACK to the Accessory
                            }
                        }
                    }
                }
                else // We are not in the middle of a packet, but we received a byte
                {
                    ESP_LOGD(__func__, "Received byte 0x%02X outside of a packet, discarding", incByte);
                }
                // Always update the previous byte
                prevByte = incByte;
            }
            if (esPodInstance->_rxIncomplete && millis() - lastByteRX > INTERBYTE_TIMEOUT) // If we are in the middle of a packet and we haven't received a byte in 1s, discard the packet
            {
                ESP_LOGW(__func__, "Packet incomplete, discarding");
                esPodInstance->_rxIncomplete = false;
                // cmd.payload = nullptr;
                // cmd.length = 0;
                // TODO: Send a NACK to the Accessory
            }
            if (millis() - lastActivity > SERIAL_TIMEOUT) // If we haven't received any byte in 30s, reset the RX state
            {
                // Reset the timestamp for next Serial timeout
                lastActivity = millis();
#ifndef NO_RESET_ON_SERIAL_TIMEOUT
                ESP_LOGW(__func__, "No activity in %lu ms, resetting RX state", SERIAL_TIMEOUT);
                esPodInstance->resetState();
#endif
            }
            vTaskDelay(pdMS_TO_TICKS(RX_TASK_INTERVAL_MS));
        }
    }
}

/// @brief Processor task retrieving from the cmdQueue and processing the commands
/// @param pvParameters
void esPod::_processTask(void *pvParameters)
{
    esPod *esPodInstance = static_cast<esPod *>(pvParameters);
    aapCommand incCmd;

#ifdef STACK_HIGH_WATERMARK_LOG
    UBaseType_t uxHighWaterMark;
    UBaseType_t minHightWaterMark = PROCESS_TASK_STACK_SIZE;
#endif

    while (true)
    {
// Stack high watermark logging
#ifdef STACK_HIGH_WATERMARK_LOG
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
        if (uxHighWaterMark < minHightWaterMark)
        {
            minHightWaterMark = uxHighWaterMark;
            ESP_LOGI("HWM", "Process Task High Watermark: %d, used stack: %d", minHightWaterMark, PROCESS_TASK_STACK_SIZE - minHightWaterMark);
        }
#endif

        // If the esPod is disabled, check the queue and purge it before jumping to the next cycle
        if (esPodInstance->disabled)
        {
            while (xQueueReceive(esPodInstance->_cmdQueue, &incCmd, 0) == pdTRUE) // Non blocking receive
            {
                // Do not process, just free the memory
                delete[] incCmd.payload;
                incCmd.payload = nullptr;
                incCmd.length = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(2 * PROCESS_INTERVAL_MS));
            continue;
        }
        if (xQueueReceive(esPodInstance->_cmdQueue, &incCmd, 0) == pdTRUE) // Non blocking receive
        {
            // Process the command
            esPodInstance->_processPacket(incCmd.payload, incCmd.length);
            // Free the memory allocated for the payload
            delete[] incCmd.payload;
            incCmd.payload = nullptr;
            incCmd.length = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(PROCESS_INTERVAL_MS));
    }
}

/// @brief Transmit task, retrieves from the txQueue and sends the packets over Serial at high priority but wider timing
/// @param pvParameters
void esPod::_txTask(void *pvParameters)
{
    esPod *esPodInstance = static_cast<esPod *>(pvParameters);
    aapCommand txCmd;

#ifdef STACK_HIGH_WATERMARK_LOG
    UBaseType_t uxHighWaterMark;
    UBaseType_t minHightWaterMark = TX_TASK_STACK_SIZE;
#endif

    while (true)
    {
// Stack high watermark logging
#ifdef STACK_HIGH_WATERMARK_LOG
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
        if (uxHighWaterMark < minHightWaterMark)
        {
            minHightWaterMark = uxHighWaterMark;
            ESP_LOGI("HWM", "TX Task High Watermark: %d, used stack: %d", minHightWaterMark, TX_TASK_STACK_SIZE - minHightWaterMark);
        }
#endif

        // If the esPod is disabled, check the queue and purge it before jumping to the next cycle
        if (esPodInstance->disabled)
        {
            while (xQueueReceive(esPodInstance->_txQueue, &txCmd, 0) == pdTRUE)
            {
                // Do not process, just free the memory
                delete[] txCmd.payload;
                txCmd.payload = nullptr;
                txCmd.length = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(TX_INTERVAL_MS));
            continue;
        }
        if (!esPodInstance->_rxIncomplete && esPodInstance->_pendingCmdId_0x00 == 0x00 && esPodInstance->_pendingCmdId_0x03 == 0x00 && esPodInstance->_pendingCmdId_0x04 == 0x00) //_rxTask is not in the middle of a packet, there isn't a valid pending for either lingoes
        {
            // Retrieve from the queue and send the packet
            if (xQueueReceive(esPodInstance->_txQueue, &txCmd, 0) == pdTRUE)
            {
                // vTaskDelay(pdMS_TO_TICKS(TX_INTERVAL_MS));
                // Send the packet
                esPodInstance->_sendPacket(txCmd.payload, txCmd.length);
                // Free the memory allocated for the payload
                delete[] txCmd.payload;
                txCmd.payload = nullptr;
                txCmd.length = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(TX_INTERVAL_MS));
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(RX_TASK_INTERVAL_MS));
        }
    }
}

/// @brief Low priority task to queue acks *outside* of the timer interrupt context
/// @param pvParameters
void esPod::_timerTask(void *pvParameters)
{
    esPod *esPodInstance = static_cast<esPod *>(pvParameters);
    TimerCallbackMessage msg;

    while (true)
    {
        if (xQueueReceive(esPodInstance->_timerQueue, &msg, 0) == pdTRUE)
        {
            if (msg.targetLingo == 0x00)
            {
                // esPodInstance->L0x00_0x02_iPodAck(iPodAck_OK, msg.cmdID);
                L0x00::_0x02_iPodAck(esPodInstance, iPodAck_OK, msg.cmdID);
            }
            else if (msg.targetLingo == 0x04)
            {
                // esPodInstance->L0x04_0x01_iPodAck(iPodAck_OK, msg.cmdID);
                L0x04::_0x01_iPodAck(esPodInstance, iPodAck_OK, msg.cmdID);
                if (msg.cmdID == esPodInstance->trackChangeAckPending)
                {
                    esPodInstance->trackChangeAckPending = 0x00;
                }
            }
            else if (msg.targetLingo == 0x03)
            {
                // esPodInstance->L0x04_0x01_iPodAck(iPodAck_OK, msg.cmdID);
                L0x03::_0x00_iPodAck(esPodInstance, iPodAck_OK, msg.cmdID);
                if (msg.cmdID == esPodInstance->trackChangeAckPending)
                {
                    esPodInstance->trackChangeAckPending = 0x00;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TIMER_INTERVAL_MS));
    }
}
#pragma endregion

#pragma region Timer Callbacks

/// @brief Callback for L0x00 pending Ack timer
/// @param xTimer
void esPod::_pendingTimerCallback_0x00(TimerHandle_t xTimer)
{
    esPod *esPodInstance = static_cast<esPod *>(pvTimerGetTimerID(xTimer));
    TimerCallbackMessage msg = {esPodInstance->_pendingCmdId_0x00, 0x00};
    xQueueSendFromISR(esPodInstance->_timerQueue, &msg, NULL);
}

/// @brief Callback for L0x03 pending Ack timer
/// @param xTimer
void esPod::_pendingTimerCallback_0x03(TimerHandle_t xTimer)
{
    esPod *esPodInstance = static_cast<esPod *>(pvTimerGetTimerID(xTimer));
    TimerCallbackMessage msg = {esPodInstance->_pendingCmdId_0x03, 0x03};
    xQueueSendFromISR(esPodInstance->_timerQueue, &msg, NULL);
}

/// @brief Callback for L0x04 pending Ack timer
/// @param xTimer
void esPod::_pendingTimerCallback_0x04(TimerHandle_t xTimer)
{
    esPod *esPodInstance = static_cast<esPod *>(pvTimerGetTimerID(xTimer));
    TimerCallbackMessage msg = {esPodInstance->_pendingCmdId_0x04, 0x04};
    xQueueSendFromISR(esPodInstance->_timerQueue, &msg, NULL);
}
#pragma endregion

//-----------------------------------------------------------------------
//|                          Packet management                          |
//-----------------------------------------------------------------------
#pragma region Packet management
/// @brief //Calculates the checksum of a packet that starts from i=0 ->Lingo to i=len -> Checksum
/// @param byteArray Array from Lingo byte to Checksum byte
/// @param len Length of array (Lingo byte to Checksum byte)
/// @return Calculated checksum for comparison
byte esPod::_checksum(const byte *byteArray, uint32_t len)
{
    uint32_t tempChecksum = len;
    for (int i = 0; i < len; i++)
    {
        tempChecksum += byteArray[i];
    }
    tempChecksum = 0x100 - (tempChecksum & 0xFF);
    return (byte)tempChecksum;
}

/// @brief Composes and sends a packet over the _targetSerial
/// @param byteArray Array to send, starting with the Lingo byte and without the checksum byte
/// @param len Length of the array to send
void esPod::_sendPacket(const byte *byteArray, uint32_t len)
{
    uint32_t finalLength = len + 4;
    byte tempBuf[finalLength] = {0x00};

    tempBuf[0] = 0xFF;
    tempBuf[1] = 0x55;
    tempBuf[2] = (byte)len;
    for (uint32_t i = 0; i < len; i++)
    {
        tempBuf[3 + i] = byteArray[i];
    }
    tempBuf[3 + len] = esPod::_checksum(byteArray, len);

    _targetSerial.write(tempBuf, finalLength);
}

/// @brief Adds a packet to the transmit queue
/// @param byteArray Array of bytes to add to the queue
/// @param len Length of data in the array
void esPod::_queuePacket(const byte *byteArray, uint32_t len)
{
    aapCommand cmdToQueue;
    cmdToQueue.payload = new byte[len];
    cmdToQueue.length = len;
    memcpy(cmdToQueue.payload, byteArray, len);
    if (xQueueSend(_txQueue, &cmdToQueue, pdMS_TO_TICKS(5)) != pdTRUE)
    {
        ESP_LOGW(__func__, "Could not queue packet");
        delete[] cmdToQueue.payload;
        cmdToQueue.payload = nullptr;
        cmdToQueue.length = 0;
    }
}

/// @brief Adds a packet to the transmit queue, but at the front for immediate processing
/// @param byteArray Array of bytes to add to the queue
/// @param len Length of data in the array
void esPod::_queuePacketToFront(const byte *byteArray, uint32_t len)
{
    aapCommand cmdToQueue;
    cmdToQueue.payload = new byte[len];
    cmdToQueue.length = len;
    memcpy(cmdToQueue.payload, byteArray, len);
    if (xQueueSendToFront(_txQueue, &cmdToQueue, pdMS_TO_TICKS(5)) != pdTRUE)
    {
        ESP_LOGW(__func__, "Could not queue packet");
        delete[] cmdToQueue.payload;
        cmdToQueue.payload = nullptr;
        cmdToQueue.length = 0;
    }
}

/// @brief Processes a valid packet and calls the relevant Lingo processor
/// @param byteArray Checksum-validated packet starting at LingoID
/// @param len Length of valid data in the packet
void esPod::_processPacket(const byte *byteArray, uint32_t len)
{
    byte rxLingoID = byteArray[0];
    const byte *subPayload = byteArray + 1; // Squeeze the Lingo out
    uint32_t subPayloadLen = len - 1;
    switch (rxLingoID) // 0x00 is general Lingo and 0x04 is extended Lingo. Nothing else is expected from the Mini
    {
    case 0x00: // General Lingo
        ESP_LOGD(IPOD_TAG, "Lingo 0x00 Packet in processor,payload length: %d", subPayloadLen);
        L0x00::processLingo(this, subPayload, subPayloadLen);
        break;

    case 0x03: // Display Remote Lingo
        ESP_LOGD(IPOD_TAG, "Lingo 0x03 Packet in processor,payload length: %d", subPayloadLen);
        L0x03::processLingo(this, subPayload, subPayloadLen);
        break;

    case 0x04: // Extended Interface Lingo
        ESP_LOGD(IPOD_TAG, "Lingo 0x04 Packet in processor,payload length: %d", subPayloadLen);
        L0x04::processLingo(this, subPayload, subPayloadLen);
        break;

    default:
        ESP_LOGW(IPOD_TAG, "Unknown Lingo packet : L0x%02x 0x%02x", rxLingoID, byteArray[1]);
        break;
    }
}
#pragma endregion

//-----------------------------------------------------------------------
//|         Constructor, reset, attachCallback for PB control           |
//-----------------------------------------------------------------------
#pragma region Constructor, destructor, reset and external PB Contoller attach
/// @brief Constructor for the esPod class
/// @param targetSerial (Serial) stream on which the esPod will be communicating
esPod::esPod(Stream &targetSerial)
    : _targetSerial(targetSerial)
{
    // Create queues with pointer structures to byte arrays
    _cmdQueue = xQueueCreate(CMD_QUEUE_SIZE, sizeof(aapCommand));
    _txQueue = xQueueCreate(TX_QUEUE_SIZE, sizeof(aapCommand));
    _timerQueue = xQueueCreate(TIMER_QUEUE_SIZE, sizeof(TimerCallbackMessage));

    if (_cmdQueue == NULL || _txQueue == NULL || _timerQueue == NULL) // Add _timerQueue check
    {
        ESP_LOGE(IPOD_TAG, "Could not create queues");
    }

    // Create FreeRTOS tasks for compiling incoming commands, processing commands and transmitting commands
    if (_cmdQueue != NULL && _txQueue != NULL && _timerQueue != NULL) // Add _timerQueue check
    {
        xTaskCreatePinnedToCore(_rxTask, "RX Task", RX_TASK_STACK_SIZE, this, RX_TASK_PRIORITY, &_rxTaskHandle, 1);
        xTaskCreatePinnedToCore(_processTask, "Processor Task", PROCESS_TASK_STACK_SIZE, this, PROCESS_TASK_PRIORITY, &_processTaskHandle, 1);
        xTaskCreatePinnedToCore(_txTask, "Transmit Task", TX_TASK_STACK_SIZE, this, TX_TASK_PRIORITY, &_txTaskHandle, 1);
        xTaskCreatePinnedToCore(_timerTask, "Timer Task", TIMER_TASK_STACK_SIZE, this, TIMER_TASK_PRIORITY, &_timerTaskHandle, 1);

        if (_rxTaskHandle == NULL || _processTaskHandle == NULL || _txTaskHandle == NULL || _timerTaskHandle == NULL)
        {
            ESP_LOGE(IPOD_TAG, "Could not create tasks");
        }
        else
        {
            _pendingTimer_0x00 = xTimerCreate("Pending Timer 0x00", pdMS_TO_TICKS(1000), pdFALSE, this, esPod::_pendingTimerCallback_0x00);
            _pendingTimer_0x03 = xTimerCreate("Pending Timer 0x03", pdMS_TO_TICKS(1000), pdFALSE, this, esPod::_pendingTimerCallback_0x03);
            _pendingTimer_0x04 = xTimerCreate("Pending Timer 0x04", pdMS_TO_TICKS(1000), pdFALSE, this, esPod::_pendingTimerCallback_0x04);
            if (_pendingTimer_0x00 == NULL || _pendingTimer_0x03 == NULL || _pendingTimer_0x04 == NULL)
            {
                ESP_LOGE(IPOD_TAG, "Could not create timers");
            }
        }
    }
    else
    {
        ESP_LOGE(IPOD_TAG, "Could not create tasks, queues not created");
    }
}

/// @brief Destructor for the esPod class. Normally not used.
esPod::~esPod()
{
    aapCommand tempCmd;
    vTaskDelete(_rxTaskHandle);
    vTaskDelete(_processTaskHandle);
    vTaskDelete(_txTaskHandle);
    vTaskDelete(_timerTaskHandle);
    // Stop timers that might be running
    stopTimer(_pendingTimer_0x00);
    stopTimer(_pendingTimer_0x03);
    stopTimer(_pendingTimer_0x04);
    xTimerDelete(_pendingTimer_0x00, 0);
    xTimerDelete(_pendingTimer_0x03, 0);
    xTimerDelete(_pendingTimer_0x04, 0);
    // Remember to deallocate memory
    while (xQueueReceive(_cmdQueue, &tempCmd, 0) == pdTRUE)
    {
        delete[] tempCmd.payload;
        tempCmd.payload = nullptr;
        tempCmd.length = 0;
    }
    while (xQueueReceive(_txQueue, &tempCmd, 0) == pdTRUE)
    {
        delete[] tempCmd.payload;
        tempCmd.payload = nullptr;
        tempCmd.length = 0;
    }
    vQueueDelete(_cmdQueue);
    vQueueDelete(_txQueue);
    vQueueDelete(_timerQueue);
}

void esPod::resetState()
{

    ESP_LOGW(IPOD_TAG, "esPod resetState called");
    // State variables
    extendedInterfaceModeActive = false;

    // Metadata variables
    trackDuration = 1;
    prevTrackDuration = 1;
    playPosition = 0;

    // Playback Engine
    playStatus = PB_STATE_PAUSED;
    playStatusNotificationState = NOTIF_OFF;
    trackChangeAckPending = 0x00;
    shuffleStatus = 0x00;
    repeatStatus = 0x02;

    // TrackList variables
    currentTrackIndex = 0;
    prevTrackIndex = TOTAL_NUM_TRACKS - 1;
    for (uint16_t i = 0; i < TOTAL_NUM_TRACKS; i++)
        trackList[i] = 0;
    trackListPosition = 0;

    // Reset the queues
    aapCommand tempCmd;

    // Remember to deallocate memory
    while (xQueueReceive(_cmdQueue, &tempCmd, 0) == pdTRUE)
    {
        delete[] tempCmd.payload;
        tempCmd.payload = nullptr;
        tempCmd.length = 0;
    }
    while (xQueueReceive(_txQueue, &tempCmd, 0) == pdTRUE)
    {
        delete[] tempCmd.payload;
        tempCmd.payload = nullptr;
        tempCmd.length = 0;
    }
    xQueueReset(_cmdQueue);
    xQueueReset(_txQueue);

    // Stop timers
    stopTimer(_pendingTimer_0x00);
    stopTimer(_pendingTimer_0x03);
    stopTimer(_pendingTimer_0x04);
    _pendingCmdId_0x00 = 0x00;
    _pendingCmdId_0x03 = 0x00;
    _pendingCmdId_0x04 = 0x00;
}

void esPod::attachPlayControlHandler(playStatusHandler_t playHandler)
{
    _playStatusHandler = playHandler;
    ESP_LOGD(IPOD_TAG, "PlayControlHandler attached.");
}
#pragma endregion

