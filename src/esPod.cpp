#include "esPod.h"

//-----------------------------------------------------------------------
//|         Constructor, reset, attachCallback for PB control           |
//-----------------------------------------------------------------------
#pragma region Constructor, destructor, reset and external PB Contoller attach

esPod::esPod(uint8_t uartNum, int rxPin, int txPin, uint32_t baud)
    : _uartPort((uart_port_t)uartNum), _rxPin(rxPin), _txPin(txPin), _baudrate(baud)
{
    // Attempt to install and initialise the UART
    if (uartNum > UART_NUM_MAX)
    {
        ESP_LOGE(__func__, "Invalid UART port number, defaulting to UART port 1");
        _uartPort = UART_NUM_1;
    }
    // Sanity check if the driver is somehow already installed
    if (uart_is_driver_installed(_uartPort))
    {
        ESP_LOGW(__func__, "UART driver is already installed for this port, desinstalling...");
        uart_driver_delete(_uartPort);
    }
    // Set up UART config
    uart_config_t uart_config = {
        .baud_rate = (int)_baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t uartRet = uart_driver_install(_uartPort, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 20, &_uartEventQueue, 0);
    if (uartRet != ESP_OK)
    {
        ESP_LOGE(__func__, "Could not install driver for esPod UART");
        // return;
    }
    uartRet = uart_param_config(_uartPort, &uart_config);
    if (uartRet != ESP_OK)
    {
        ESP_LOGE(__func__, "Could not configure the UART port for esPod");
        // return;
    }
    uartRet = uart_set_pin(_uartPort, _txPin, _rxPin, -1, -1);
    if (uartRet != ESP_OK)
    {
        ESP_LOGE(__func__, "Could not set the pins for the esPod UART");
        // return;
    }
    // Trigger autobaud if necessary
    if (_baudrate == 0)
    {
        ESP_LOGI(__func__, "Autobaud starting...");
        uartRet = uart_detect_bitrate_start(_uartPort, NULL);
        if (uartRet != ESP_OK)
        {
            ESP_LOGE(__func__, "Could not set up autobaud : %s", esp_err_to_name(uartRet));
        }
        _isBaudReady = false;
    }
    else
        _isBaudReady = true;

    // Attempt to initialise the FreeRTOS objects
    if (_initFreeRTOSStack() != ESP_OK)
    {
        ESP_LOGE(__func__, "Impossible to initialise the esPod object, returning...");
        return;
    }
}

// Not used yet
// esPod *esPod::createWithAutobaud(uint8_t uartNum, int rxPin, int txPin)
// {
//     return nullptr;
// }

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
    if (_cmdRingBuffer != NULL) // Buffer is direct data, not pointers to the heap. Can directly deallocate it
        vRingbufferDelete(_cmdRingBuffer);

    while (xQueueReceive(_txQueue, &tempCmd, 0) == pdTRUE) // This queue is full of pointers to heap. Gotta free the heap first
    {
        delete[] tempCmd.payload;
        tempCmd.payload = nullptr;
        tempCmd.length = 0;
    }
    vQueueDelete(_txQueue);
    vQueueDelete(_timerQueue);
}

void esPod::resetState()
{

    ESP_LOGW(__func__, "esPod resetState called");

    _rxIncomplete = false;

    // State variables
    extendedInterfaceModeActive = false;

    // Flags for track change management
    _albumNameUpdated = false;
    _artistNameUpdated = false;
    _trackTitleUpdated = false;
    _trackDurationUpdated = false;

    // Playback Engine
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
    while (xQueueReceive(_txQueue, &tempCmd, 0) == pdTRUE)
    {
        if (tempCmd.payload != nullptr)
        {
            xQueueSend(_txFreeBufferQueue, &tempCmd.payload, 0);
        }
    }

    // Reset Ring buffer
    size_t tempSize;
    void *tempItem;
    while ((tempItem = xRingbufferReceive(_cmdRingBuffer, &tempSize, 0)) != NULL)
    {
        vRingbufferReturnItem(_cmdRingBuffer, tempItem);
    }

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
    ESP_LOGD(__func__, "PlayControlHandler attached.");
}

void esPod::play(bool noLoop)
{
    playStatus = PB_STATE_PLAYING;
    if (!noLoop && (_playStatusHandler != NULL))
    {
        _playStatusHandler(PB_CMD_PLAY);
    }

    ESP_LOGD(__func__, "esPod set to play.");
}

void esPod::pause(bool noLoop)
{
    playStatus = PB_STATE_PAUSED;
    if (!noLoop && (_playStatusHandler != NULL))
    {
        _playStatusHandler(PB_CMD_PAUSE);
    }
    ESP_LOGD(__func__, "esPod paused.");
}

void esPod::stop(bool noLoop)
{
    playStatus = PB_STATE_STOPPED;
    if (!noLoop && (_playStatusHandler != NULL))
    {
        _playStatusHandler(PB_CMD_STOP);
    }
    ESP_LOGD(__func__, " esPod stopped.");
}

void esPod::updatePlayPosition(uint32_t position)
{
    playPosition = position;
    if (playStatusNotificationState == NOTIF_ON && trackChangeAckPending == 0x00)
        L0x04::_0x27_PlayStatusNotification(this, 0x04, position);
}

void esPod::updateAlbumName(const char *incAlbumName)
{
    if (trackChangeAckPending > 0x00) // There is a pending track change ack
    {
        if (!_albumNameUpdated)
        {
            strcpy(albumName, incAlbumName);
            _albumNameUpdated = true;
            ESP_LOGD(__func__, "Album name update to %s", albumName);
        }
        else
            ESP_LOGD(__func__, "Album name already updated to %s", albumName);
    }
    else // There is no pending track change ack : change is coming from phone/AVRC target
    {
        if (strcmp(incAlbumName, albumName) != 0) // New Album Name
        {
            strcpy(prevAlbumName, albumName); // Preserve the previous album name
            strcpy(albumName, incAlbumName);  // Copy new album name
            _albumNameUpdated = true;
            ESP_LOGD(__func__, "Album name updated to %s", albumName);
        }
        else // Not new album name
            ESP_LOGD(__func__, "Album name already updated to %s", albumName);
    }
    _checkAllMetaUpdated();
}

void esPod::updateArtistName(const char *incArtistName)
{
    if (trackChangeAckPending > 0x00) // There is a pending track change ack
    {
        if (!_artistNameUpdated)
        {
            strcpy(artistName, incArtistName);
            _artistNameUpdated = true;
            ESP_LOGD(__func__, "Artist name update to %s", artistName);
        }
        else
            ESP_LOGD(__func__, "Artist name already updated to %s", artistName);
    }
    else // There is no pending track change ack : change is coming from phone/AVRC target
    {
        if (strcmp(incArtistName, artistName) != 0) // New Artist Name
        {
            strcpy(prevArtistName, artistName); // Preserve the previous artist name
            strcpy(artistName, incArtistName);  // Copy new artist name
            _artistNameUpdated = true;
            ESP_LOGD(__func__, "Artist name updated to %s", artistName);
        }
        else // Not new artist name
            ESP_LOGD(__func__, "Artist name already updated to %s", artistName);
    }
    _checkAllMetaUpdated();
}

void esPod::updateTrackTitle(const char *incTrackTitle)
{
    if (trackChangeAckPending > 0x00) // There is a pending metadata update
    {
        if (!_trackTitleUpdated) // Track title not yet updated
        {
            strcpy(trackTitle, incTrackTitle);
            _trackTitleUpdated = true;
            ESP_LOGD(__func__, "Title update to %s", trackTitle);
        }
        else
            ESP_LOGD(__func__, "Title already updated to %s", trackTitle);
    }
    else // There is no pending track change ack : change is coming from phone/AVRC target.
    {
        if (strcmp(incTrackTitle, trackTitle) != 0) // New track title, we assume it's a NEXT track because otherwise the comparison logic becomes heavy
        {
            // Assume it is Next, perform cursor operations
            trackListPosition = (trackListPosition + 1) % TOTAL_NUM_TRACKS;
            prevTrackIndex = currentTrackIndex;
            currentTrackIndex = (currentTrackIndex + 1) % TOTAL_NUM_TRACKS;
            trackList[trackListPosition] = (currentTrackIndex);

            strcpy(prevTrackTitle, trackTitle); // Preserve the previous track title
            strcpy(trackTitle, incTrackTitle);  // Update the new track title
            _trackTitleUpdated = true;
            ESP_LOGD(__func__, "Title update to %s", trackTitle);
        }
        else // Track title is identical, no movement
        {
            ESP_LOGD(__func__, "Title already updated to : %s", trackTitle);
        }
    }
    _checkAllMetaUpdated();
}

void esPod::updateTrackDuration(uint32_t incTrackDuration)
{
    if (trackChangeAckPending > 0x00) // There is a pending metadata update
    {
        if (!_trackDurationUpdated) // Track duration not yet updated
        {
            trackDuration = incTrackDuration;
            _trackDurationUpdated = true;
            ESP_LOGD(__func__, "Track duration updated to %d", trackDuration);
        }
        else
            ESP_LOGD(__func__, "Track duration already updated to %d", trackDuration);
    }
    else // There is no pending track change ack : change is coming from phone/AVRC target.
    {
        if (incTrackDuration != trackDuration) // Different incoming metadata
        {
            prevTrackDuration = trackDuration; // Preserve the current value
            trackDuration = incTrackDuration;  // Then updated it
            _trackDurationUpdated = true;
            ESP_LOGD(__func__, "Track duration updated to %d", trackDuration);
        }
        else
            ESP_LOGD(__func__, "Track duration already updated to %d", trackDuration);
    }
    _checkAllMetaUpdated();
}

void esPod::_checkAllMetaUpdated()
{
    if (_albumNameUpdated && _artistNameUpdated && _trackTitleUpdated && _trackDurationUpdated)
    {
        // If all fields have received at least one update and the
        // trackChangeAckPending is still hanging. The failsafe for this one is
        // in the espod _processTask
        if (trackChangeAckPending > 0x00)
        {
            ESP_LOGD(__func__, "Artist+Album+Title+Duration +++ ACK Pending 0x%x\n\tPending duration: %d", trackChangeAckPending, millis() - trackChangeTimestamp);
            if (trackChangeAckPending == 0x11)
            {
                L0x03::_0x00_iPodAck(this, iPodAck_OK, trackChangeAckPending);
            }
            else
            {
                L0x04::_0x01_iPodAck(this, iPodAck_OK, trackChangeAckPending);
            }
            trackChangeAckPending = 0x00;
            ESP_LOGD(__func__, "trackChangeAckPending reset to 0x00");
        }
        _albumNameUpdated = false;
        _artistNameUpdated = false;
        _trackTitleUpdated = false;
        _trackDurationUpdated = false;
        ESP_LOGD(__func__, "Artist+Album+Title+Duration : True -> False");
        // Inform the car
        if (playStatusNotificationState == NOTIF_ON)
        {
            L0x04::_0x27_PlayStatusNotification(this, 0x01, currentTrackIndex);
        }
    }
}

#pragma endregion

//-----------------------------------------------------------------------
//|                      Cardinal tasks and Timers                      |
//-----------------------------------------------------------------------
#pragma region Tasks

esp_err_t esPod::_initFreeRTOSStack()
{
    esp_err_t ret = ESP_OK;
    // Create queues with pointer structures to byte arrays

    _cmdRingBuffer = xRingbufferCreate(CMD_RING_BUF_SIZE, RINGBUF_TYPE_NOSPLIT);
    _txQueue = xQueueCreate(TX_QUEUE_SIZE, sizeof(aapCommand));
    _txFreeBufferQueue = xQueueCreate(TX_QUEUE_SIZE, sizeof(byte *));
    _timerQueue = xQueueCreate(TIMER_QUEUE_SIZE, sizeof(TimerCallbackMessage));

    // Pre-fill _txFreeBufferQueue with valid section points
    if (_txFreeBufferQueue != NULL)
    {
        for (int i = 0; i < TX_QUEUE_SIZE; i++)
        {
            byte *sectionPointer = _txBufferPool[i];
            if (xQueueSend(_txFreeBufferQueue, &sectionPointer, 0) != pdTRUE)
                ESP_LOGE(__func__, "Error initialising tx section pointers Queue!");
        }
    }

    if (_txQueue == NULL || _timerQueue == NULL || _cmdRingBuffer == NULL || _txFreeBufferQueue == NULL) // If one failed to create
    {
        ESP_LOGE(__func__, "Could not create queues/ring buffers, tasks will not be created");
        ret = ESP_FAIL;
    }

    else // If none of them are null, create the tasks
    {
        xTaskCreatePinnedToCore(_rxTask, "RX Task", RX_TASK_STACK_SIZE, this, RX_TASK_PRIORITY, &_rxTaskHandle, 1);
        xTaskCreatePinnedToCore(_processTask, "Processor Task", PROCESS_TASK_STACK_SIZE, this, PROCESS_TASK_PRIORITY, &_processTaskHandle, 1);
        xTaskCreatePinnedToCore(_txTask, "Transmit Task", TX_TASK_STACK_SIZE, this, TX_TASK_PRIORITY, &_txTaskHandle, 1);
        xTaskCreatePinnedToCore(_timerTask, "Timer Task", TIMER_TASK_STACK_SIZE, this, TIMER_TASK_PRIORITY, &_timerTaskHandle, 1);

        if (_rxTaskHandle == NULL || _processTaskHandle == NULL || _txTaskHandle == NULL || _timerTaskHandle == NULL)
        {
            ESP_LOGE(__func__, "Could not create tasks");
            ret = ESP_FAIL;
        }

        else
        {
            _pendingTimer_0x00 = xTimerCreate("Pending Timer 0x00", pdMS_TO_TICKS(1000), pdFALSE, this, esPod::_pendingTimerCallback_0x00);
            _pendingTimer_0x03 = xTimerCreate("Pending Timer 0x03", pdMS_TO_TICKS(1000), pdFALSE, this, esPod::_pendingTimerCallback_0x03);
            _pendingTimer_0x04 = xTimerCreate("Pending Timer 0x04", pdMS_TO_TICKS(1000), pdFALSE, this, esPod::_pendingTimerCallback_0x04);
            if (_pendingTimer_0x00 == NULL || _pendingTimer_0x03 == NULL || _pendingTimer_0x04 == NULL)
            {
                ESP_LOGE(__func__, "Could not create timers");
                ret = ESP_FAIL;
            }
        }
    }
    return ret;
}

void esPod::_rxTask(void *pvParameters)
{
    esPod *inst = static_cast<esPod *>(pvParameters);
    uart_event_t uartEvent;
    size_t bufferedSize;
    byte prevByte = 0x00;
    byte incByte = 0x00;
    byte buf[MAX_PACKET_SIZE] = {0x00};
    uint32_t expLength = 0;
    uint32_t cursor = 0;
    TickType_t waitTime = inst->_isBaudReady ? portMAX_DELAY : pdMS_TO_TICKS(1000);
    bool serialTimedOut = false;
    unsigned long lastByteRX = millis();   // Last time a byte was RXed in a packet
    unsigned long lastActivity = millis(); // Last time any RX activity was detected

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

        // If the baudrate is somewhat set
        if (inst->_isBaudReady)
        {
            // Adjust the waiting time to interbyte timeout if the packet transmission is interrupted, otherwise hardware timeout
            waitTime = inst->_rxIncomplete ? pdMS_TO_TICKS(INTERBYTE_TIMEOUT) : pdMS_TO_TICKS(SERIAL_TIMEOUT);
            waitTime = serialTimedOut ? portMAX_DELAY : waitTime;
        }
        if (xQueueReceive(inst->_uartEventQueue, (void *)&uartEvent, waitTime) == pdTRUE)
        {
            serialTimedOut = false;
            switch (uartEvent.type)
            {
            case UART_DATA:
            {
                // Retrieve buffered size directly from event information
                bufferedSize = uartEvent.size;
                // uart_get_buffered_data_len(inst->_uartPort, &bufferedSize);

                // Instance is disabled
                if (inst->disabled)
                {
                    uart_flush_input(inst->_uartPort);
                    break;
                }

                while (bufferedSize--)
                {
                    uart_read_bytes(inst->_uartPort, &incByte, 1, 0);
                    // If we are not in the middle of a RX, and we receive a 0xFF 0x55, start sequence, reset expected length and position cursor
                    if (prevByte == 0xFF && incByte == 0x55 && !inst->_rxIncomplete)
                    {
                        inst->_rxIncomplete = true;
                        cursor = 0;
                        expLength = 0;
                    }
                    else if (inst->_rxIncomplete) // Mid-packet
                    {
                        // Expected length has not been received yet
                        if (expLength == 0 && cursor == 0)
                        {
                            expLength = incByte; // First byte after 0xFF 0x55
                            if (expLength > MAX_PACKET_SIZE)
                            {
                                ESP_LOGW(__func__, "Expected length is too long, discarding packet");
                                inst->_rxIncomplete = false;
                                // TODO: Send a NACK to the Accessory
                            }
                            else if (expLength == 0)
                            {
                                ESP_LOGW(__func__, "Expected length is 0, discarding packet");
                                inst->_rxIncomplete = false;
                                // TODO: Send a NACK to the Accessory
                            }
                        }
                        else // Length is already received
                        {
                            buf[cursor++] = incByte;
                            if (cursor == expLength + 1)
                            {
                                // We have received the expected length + checksum
                                inst->_rxIncomplete = false;
                                // Check the checksum
                                byte calcChecksum = esPod::_checksum(buf, expLength);
                                if (calcChecksum == incByte) // Checksum OK
                                {
                                    // Checksum is correct, send the packet to the processing queue
                                    // RingBuffer method
                                    if (xRingbufferSend(inst->_cmdRingBuffer, buf, expLength, pdMS_TO_TICKS(5)) != pdTRUE)
                                        ESP_LOGW(__func__, "Ringbuffer full, dropping packet");
                                    else
                                        ESP_LOGD(__func__, "Packet sent to RingBuffer");
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
            }
            break;

            case UART_BREAK:
                if (inst->_isBaudReady)
                    ESP_LOGE(__func__, "UART BREAK event");
                break;

            case UART_BUFFER_FULL:
                if (inst->_isBaudReady)
                    ESP_LOGE(__func__, "UART BUFFER FULL event");
                break;

            case UART_FIFO_OVF:
                if (inst->_isBaudReady)
                    ESP_LOGE(__func__, "UART FIFO overflow event");
                break;

            case UART_FRAME_ERR:
                if (inst->_isBaudReady)
                    ESP_LOGE(__func__, "UART Frame error event");
                break;

            case UART_PARITY_ERR:
                if (inst->_isBaudReady)
                    ESP_LOGE(__func__, "UART PARITY error event");
                break;

            default:
                ESP_LOGW(__func__, "UART event : %d", uartEvent.type);
                break;
            }
        }
        else // Nothing came in the queue in time, handle the various timeouts
        {
            if (!inst->_isBaudReady) // Baudrate was not yet ready
            {
                uart_bitrate_res_t bitrateDetResults;
                esp_err_t uartDetErr = uart_detect_bitrate_stop(inst->_uartPort, false, &bitrateDetResults);
                if (uartDetErr != ESP_OK)
                {
                    ESP_LOGE(__func__, "Error in the bitrate detection : %s", esp_err_to_name(uartDetErr));
                    uart_set_baudrate(inst->_uartPort, 19200);
                    inst->_isBaudReady = true;
                }
                else if (bitrateDetResults.edge_cnt > 0)
                {
                    uint32_t baudrate = bitrateDetResults.clk_freq_hz * 2 / (bitrateDetResults.low_period + bitrateDetResults.high_period);
                    ESP_LOGI(__func__, "Baudrate detected : %lu", baudrate);
                    uart_set_baudrate(inst->_uartPort, baudrate);
                    inst->_isBaudReady = true;
                }
                else
                {
                    ESP_LOGW(__func__, "Restarting bitrate detection");
                    uart_detect_bitrate_start(inst->_uartPort, NULL);
                }
            }
            else if (inst->_rxIncomplete)
            {
                ESP_LOGW(__func__, "Packet incomplete, discarding");
                inst->_rxIncomplete = false;
                // cmd.payload = nullptr;
                // cmd.length = 0;
                // TODO: Send a NACK to the Accessory
            }
            else // Serial timeout, no data on the line
            {
#ifndef NO_RESET_ON_SERIAL_TIMEOUT
                ESP_LOGW(__func__, "No activity in %lu ms, resetting RX state", SERIAL_TIMEOUT);
                inst->resetState();
#else
                ESP_LOGW(__func__, "No activity in %lu ms, no reset.");
#endif
                serialTimedOut = true;
            }
        }

        /*
        // If the esPod is disabled, flush the RX buffer and wait for 2*RX_TASK_INTERVAL_MS before checking again
        if (inst->disabled)
        {
            while (inst->_targetSerial.available())
            {
                inst->_targetSerial.read();
            }
            vTaskDelay(pdMS_TO_TICKS(2 * RX_TASK_INTERVAL_MS)); // Watchdog necessity
            continue;
        }
        else // esPod is enabled, process away !
        {
            // Use of while instead of if()
            while (inst->_targetSerial.available())
            {
                // Timestamping the last activity on RX
                lastActivity = millis();
                incByte = inst->_targetSerial.read();
                // If we are not in the middle of a RX, and we receive a 0xFF 0x55, start sequence, reset expected length and position cursor
                if (prevByte == 0xFF && incByte == 0x55 && !inst->_rxIncomplete)
                {
                    lastByteRX = millis();
                    inst->_rxIncomplete = true;
                    expLength = 0;
                    cursor = 0;
                }
                else if (inst->_rxIncomplete)
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
                            inst->_rxIncomplete = false;
                            // TODO: Send a NACK to the Accessory
                        }
                        else if (expLength == 0)
                        {
                            ESP_LOGW(__func__, "Expected length is 0, discarding packet");
                            inst->_rxIncomplete = false;
                            // TODO: Send a NACK to the Accessory
                        }
                    }
                    else // Length is already received
                    {
                        buf[cursor++] = incByte;
                        if (cursor == expLength + 1)
                        {
                            // We have received the expected length + checksum
                            inst->_rxIncomplete = false;
                            // Check the checksum
                            byte calcChecksum = esPod::_checksum(buf, expLength);
                            if (calcChecksum == incByte)
                            {
                                // Checksum is correct, send the packet to the processing queue
                                // RingBuffer method
                                if (xRingbufferSend(inst->_cmdRingBuffer, buf, expLength, pdMS_TO_TICKS(5)) != pdTRUE)
                                    ESP_LOGW(__func__, "Ringbuffer full, dropping packet");
                                else
                                    ESP_LOGD(__func__, "Packet sent to RingBuffer");
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
            if (inst->_rxIncomplete && millis() - lastByteRX > INTERBYTE_TIMEOUT) // If we are in the middle of a packet and we haven't received a byte in 1s, discard the packet
            {
                ESP_LOGW(__func__, "Packet incomplete, discarding");
                inst->_rxIncomplete = false;
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
                inst->resetState();
#endif
            }
            vTaskDelay(pdMS_TO_TICKS(RX_TASK_INTERVAL_MS));
        }
            */
    }
}

void esPod::_processTask(void *pvParameters)
{
    esPod *inst = static_cast<esPod *>(pvParameters);
    size_t incCmdSize;

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
        byte *incCmd = (byte *)xRingbufferReceive(inst->_cmdRingBuffer, &incCmdSize, portMAX_DELAY);

        if (incCmd != NULL)
        {
            if (!inst->disabled)
                inst->_processPacket(incCmd, incCmdSize);
            vRingbufferReturnItem(inst->_cmdRingBuffer, (void *)incCmd);
        }
    }
}

void esPod::_txTask(void *pvParameters)
{
    esPod *inst = static_cast<esPod *>(pvParameters);
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

        // Retrieve from the queue and send the packet
        if (xQueueReceive(inst->_txQueue, &txCmd, portMAX_DELAY) == pdTRUE)
        {
            if (!inst->disabled)
            {
                while (!(
                    !inst->_rxIncomplete &&
                    inst->_pendingCmdId_0x00 == 0x00 &&
                    inst->_pendingCmdId_0x03 == 0x00 &&
                    inst->_pendingCmdId_0x04 == 0x00))
                {
                    // Yield until true
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                // Send the packet
                inst->_sendPacket(txCmd.payload, txCmd.length);
            }
            // Free the memory allocated for the payload, return free section pointer.
            xQueueSend(inst->_txFreeBufferQueue, &txCmd.payload, 0);
        }
    }
}

void esPod::_timerTask(void *pvParameters)
{
    esPod *inst = static_cast<esPod *>(pvParameters);
    TimerCallbackMessage msg;

    while (true)
    {
        if (xQueueReceive(inst->_timerQueue, &msg, portMAX_DELAY) == pdTRUE)
        {
            if (msg.targetLingo == 0x00)
            {
                // inst->L0x00_0x02_iPodAck(iPodAck_OK, msg.cmdID);
                L0x00::_0x02_iPodAck(inst, iPodAck_OK, msg.cmdID);
            }
            else if (msg.targetLingo == 0x04)
            {
                // inst->L0x04_0x01_iPodAck(iPodAck_OK, msg.cmdID);
                L0x04::_0x01_iPodAck(inst, iPodAck_OK, msg.cmdID);
                if (msg.cmdID == inst->trackChangeAckPending)
                {
                    inst->trackChangeAckPending = 0x00;
                    inst->_albumNameUpdated = false;
                    inst->_artistNameUpdated = false;
                    inst->_trackTitleUpdated = false;
                    inst->_trackDurationUpdated = false;
                }
            }
            else if (msg.targetLingo == 0x03)
            {
                // inst->L0x04_0x01_iPodAck(iPodAck_OK, msg.cmdID);
                L0x03::_0x00_iPodAck(inst, iPodAck_OK, msg.cmdID);
                if (msg.cmdID == inst->trackChangeAckPending)
                {
                    inst->trackChangeAckPending = 0x00;
                    inst->_albumNameUpdated = false;
                    inst->_artistNameUpdated = false;
                    inst->_trackTitleUpdated = false;
                    inst->_trackDurationUpdated = false;
                }
            }
        }
    }
}
#pragma endregion

#pragma region Timer Callbacks

void esPod::_pendingTimerCallback_0x00(TimerHandle_t xTimer)
{
    esPod *inst = static_cast<esPod *>(pvTimerGetTimerID(xTimer));
    TimerCallbackMessage msg = {inst->_pendingCmdId_0x00, 0x00};
    xQueueSendFromISR(inst->_timerQueue, &msg, NULL);
}

void esPod::_pendingTimerCallback_0x03(TimerHandle_t xTimer)
{
    esPod *inst = static_cast<esPod *>(pvTimerGetTimerID(xTimer));
    TimerCallbackMessage msg = {inst->_pendingCmdId_0x03, 0x03};
    xQueueSendFromISR(inst->_timerQueue, &msg, NULL);
}

void esPod::_pendingTimerCallback_0x04(TimerHandle_t xTimer)
{
    esPod *inst = static_cast<esPod *>(pvTimerGetTimerID(xTimer));
    TimerCallbackMessage msg = {inst->_pendingCmdId_0x04, 0x04};
    xQueueSendFromISR(inst->_timerQueue, &msg, NULL);
}
#pragma endregion

//-----------------------------------------------------------------------
//|                          Packet management                          |
//-----------------------------------------------------------------------
#pragma region Packet management

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

    uart_write_bytes(_uartPort, tempBuf, finalLength);
    // _targetSerial.write(tempBuf, finalLength);
}

void esPod::_queuePacket(const byte *byteArray, uint32_t len)
{
    byte *freeBuffer = nullptr;
    // Retrieve next available free section pointer
    if (xQueueReceive(_txFreeBufferQueue, &freeBuffer, 0) == pdTRUE)
    {
        ESP_LOGD(__func__, "Retrieved pointer to section : %p", freeBuffer);
        memcpy(freeBuffer, byteArray, len);
        aapCommand cmdToQueue;
        cmdToQueue.payload = freeBuffer;
        cmdToQueue.length = len;
        if (xQueueSend(_txQueue, &cmdToQueue, pdMS_TO_TICKS(5)) != pdTRUE)
        {
            ESP_LOGW(__func__, "Could not queue packet");
            // Return the pointer to the free section, will be overwritten later
            xQueueSend(_txFreeBufferQueue, &freeBuffer, 0);
        }
    }
    else
    {
        ESP_LOGE(__func__, "Could not retrieve pointer to free section, packet is dropped!");
    }
}

void esPod::_queuePacketToFront(const byte *byteArray, uint32_t len)
{
    byte *freeBuffer = nullptr;
    // Retrieve next available free section pointer
    if (xQueueReceive(_txFreeBufferQueue, &freeBuffer, 0) == pdTRUE)
    {
        ESP_LOGD(__func__, "Retrieved pointer to section : %p", freeBuffer);
        memcpy(freeBuffer, byteArray, len);
        aapCommand cmdToQueue;
        cmdToQueue.payload = freeBuffer;
        cmdToQueue.length = len;
        if (xQueueSendToFront(_txQueue, &cmdToQueue, pdMS_TO_TICKS(5)) != pdTRUE)
        {
            ESP_LOGW(__func__, "Could not queue packet");
            // Return the pointer to the free section, will be overwritten later
            xQueueSend(_txFreeBufferQueue, &freeBuffer, 0);
        }
    }
    else
    {
        ESP_LOGE(__func__, "Could not retrieve pointer to free section, packet is dropped!");
    }
}

void esPod::_processPacket(const byte *byteArray, size_t len)
{
    byte rxLingoID = byteArray[0];
    const byte *subPayload = byteArray + 1; // Squeeze the Lingo out
    uint32_t subPayloadLen = (uint32_t)len - 1;
    switch (rxLingoID) // 0x00 is general Lingo and 0x04 is extended Lingo. Nothing else is expected from the Mini
    {
    case 0x00: // General Lingo
        ESP_LOGD(__func__, "Lingo 0x00 Packet in processor,payload length: %d", subPayloadLen);
        L0x00::processLingo(this, subPayload, subPayloadLen);
        break;

    case 0x03: // Display Remote Lingo
        ESP_LOGD(__func__, "Lingo 0x03 Packet in processor,payload length: %d", subPayloadLen);
        L0x03::processLingo(this, subPayload, subPayloadLen);
        break;

    case 0x04: // Extended Interface Lingo
        ESP_LOGD(__func__, "Lingo 0x04 Packet in processor,payload length: %d", subPayloadLen);
        L0x04::processLingo(this, subPayload, subPayloadLen);
        break;

    default:
        ESP_LOGW(__func__, "Unknown Lingo packet : L0x%02x 0x%02x", rxLingoID, byteArray[1]);
        break;
    }
}
#pragma endregion
