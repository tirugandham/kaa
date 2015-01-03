/*
 * Copyright 2014 CyberVision, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef KAA_DISABLE_FEATURE_LOGGING

#include "kaa_logging.h"

#include <stddef.h>
#include <string.h>

#include "collections/kaa_list.h"
#include "kaa_common.h"
#include "kaa_status.h"
#include "kaa_channel_manager.h"
#include "kaa_platform_utils.h"
#include "kaa_platform_common.h"
#include "utilities/kaa_mem.h"
#include "utilities/kaa_log.h"

#include "avro_src/avro/io.h"

#define KAA_LOGGING_RECEIVE_UPDATES_FLAG   0x01
#define KAA_MAX_PADDING_LENGTH             3



extern kaa_sync_handler_fn kaa_channel_manager_get_sync_handler(kaa_channel_manager_t *self, kaa_service_t service_type);



typedef enum {
    LOGGING_RESULT_SUCCESS = 0x00,
    LOGGING_RESULT_FAILURE = 0x01
} logging_sync_result_t;



struct kaa_log_collector {
    uint16_t                     log_bucket_id;
    ext_log_storage_t           *log_storage;
    ext_log_upload_strategy_t   *log_upload_strategy;
    kaa_log_upload_properties_t  log_properties;
    kaa_status_t                *status;
    kaa_channel_manager_t       *channel_manager;
    kaa_logger_t                *logger;
};



static const kaa_service_t logging_sync_services[1] = {KAA_SERVICE_LOGGING};



kaa_error_t kaa_log_collector_create(kaa_log_collector_t **log_collector_p
                                   , kaa_status_t *status
                                   , kaa_channel_manager_t *channel_manager
                                   , kaa_logger_t *logger)
{
    KAA_RETURN_IF_NIL(log_collector_p, KAA_ERR_BADPARAM);
    kaa_log_collector_t * collector = (kaa_log_collector_t *) KAA_CALLOC(1, sizeof(kaa_log_collector_t));
    KAA_RETURN_IF_NIL(collector, KAA_ERR_NOMEM);

    collector->log_bucket_id   = 0;
    collector->status          = status;
    collector->channel_manager = channel_manager;
    collector->logger          = logger;

    *log_collector_p = collector;
    return KAA_ERR_NONE;
}



void kaa_log_collector_destroy(kaa_log_collector_t *self)
{
    if (self) {
        ext_log_storage_release(self->log_storage);
        KAA_FREE(self);
    }
}



kaa_error_t kaa_logging_init(kaa_log_collector_t *self
                           , ext_log_storage_t *storage
                           , ext_log_upload_strategy_t *upload_strategy
                           , const kaa_log_upload_properties_t *properties)
{
    KAA_RETURN_IF_NIL4(self, storage, properties, upload_strategy, KAA_ERR_BADPARAM);

    ext_log_storage_release(self->log_storage);

    self->log_storage = storage;
    self->log_properties = *properties;
    self->log_upload_strategy = upload_strategy;

    KAA_LOG_DEBUG(self->logger, KAA_ERR_NONE, "Initialized log collector with: "
                "log storage {%p}, log properties {%p}, log upload strategy {%p}"
            , storage, properties, upload_strategy);

    return KAA_ERR_NONE;
}



static void update_storage(kaa_log_collector_t *self)
{
    switch (ext_log_upload_strategy_decide(self->log_upload_strategy, self->log_storage)) {
        case CLEANUP:
            KAA_LOG_WARN(self->logger, KAA_ERR_NONE, "Initiating log storage cleanup (max allowed volume %zu; current size %zu)"
                    , self->log_properties.max_log_storage_volume
                    , ext_log_storage_get_total_size(self->log_storage)
                    );
            kaa_error_t error = ext_log_storage_shrink_to_size(self->log_storage, self->log_properties.max_log_storage_volume);
            if (error)
                KAA_LOG_ERROR(self->logger, error, "Failed to cleanup log storage");
            break;
        case UPLOAD: {
            KAA_LOG_INFO(self->logger, KAA_ERR_NONE, "Initiating log upload...");
            kaa_sync_handler_fn sync = kaa_channel_manager_get_sync_handler(self->channel_manager, logging_sync_services[0]);
            if (sync)
                (*sync)(logging_sync_services, 1);
            break;
        }
        default:
            KAA_LOG_TRACE(self->logger, KAA_ERR_NONE, "Upload will not be triggered now.");
            break;
     }
}



kaa_error_t kaa_logging_add_record(kaa_log_collector_t *self, kaa_user_log_record_t *entry)
{
    KAA_RETURN_IF_NIL2(self, entry, KAA_ERR_BADPARAM);
    KAA_RETURN_IF_NIL(self->log_storage, KAA_ERR_NOT_INITIALIZED);

    KAA_LOG_DEBUG(self->logger, KAA_ERR_NONE, "Adding new log record {%p}", entry);

    kaa_log_record_t record = { NULL, entry->get_size(entry) };
    KAA_LOG_TRACE(self->logger, KAA_ERR_NONE, "Record size is %d", record.size);
    if (!record.size)
        return KAA_ERR_BADDATA;

    kaa_error_t error = ext_log_storage_allocate_log_record_buffer(self->log_storage, &record);
    if (error)
        return error;

    avro_writer_t writer = avro_writer_memory((char *)record.data, record.size);
    if (!writer) {
        ext_log_storage_deallocate_log_record_buffer(self->log_storage, &record);
        return KAA_ERR_NOMEM;
    }

    entry->serialize(writer, entry);
    avro_writer_free(writer);

    KAA_LOG_TRACE(self->logger, KAA_ERR_NONE, "Adding serialized record to the log storage");
    error = ext_log_storage_add_log_record(self->log_storage, &record);
    if (error) {
        KAA_LOG_ERROR(self->logger, error, "Failed to add log record to storage");
        ext_log_storage_deallocate_log_record_buffer(self->log_storage, &record);
        return error;
    }

    update_storage(self);
    return KAA_ERR_NONE;
}



kaa_error_t kaa_logging_request_get_size(kaa_log_collector_t *self, size_t *expected_size)
{
    KAA_RETURN_IF_NIL2(self, expected_size, KAA_ERR_BADPARAM);
    KAA_RETURN_IF_NIL(self->log_storage, KAA_ERR_NOT_INITIALIZED);

    *expected_size = KAA_EXTENSION_HEADER_SIZE;
    *expected_size += sizeof(uint32_t); // request id + log records count

    size_t records_count = ext_log_storage_get_records_count(self->log_storage);
    size_t total_size = ext_log_storage_get_total_size(self->log_storage);

    size_t actual_size = records_count * sizeof(uint32_t) + records_count * KAA_MAX_PADDING_LENGTH + total_size;
    *expected_size += ((actual_size < self->log_properties.max_log_bucket_size)
            ? actual_size
            : self->log_properties.max_log_bucket_size);

    return KAA_ERR_NONE;
}



kaa_error_t kaa_logging_request_serialize(kaa_log_collector_t *self, kaa_platform_message_writer_t *writer)
{
    KAA_RETURN_IF_NIL2(self, writer, KAA_ERR_BADPARAM);
    KAA_RETURN_IF_NIL(self->log_storage, KAA_ERR_NOT_INITIALIZED);

    KAA_LOG_TRACE(self->logger, KAA_ERR_NONE, "Going to compile log client sync");

    kaa_platform_message_writer_t tmp_writer = *writer;

    char *extension_size_p = tmp_writer.current + sizeof(uint32_t); // Pointer to the extension size. Will be filled in later.
    kaa_error_t error = kaa_platform_message_write_extension_header(&tmp_writer, KAA_LOGGING_EXTENSION_TYPE, KAA_LOGGING_RECEIVE_UPDATES_FLAG, 0);
    if (error) {
        KAA_LOG_ERROR(self->logger, error, "Failed to write log extension header");
        return KAA_ERR_WRITE_FAILED;
    }

    if (!self->log_bucket_id && kaa_status_get_log_bucket_id(self->status, &self->log_bucket_id))
        return KAA_ERR_BAD_STATE;
    ++self->log_bucket_id;

    *((uint16_t *) tmp_writer.current) = KAA_HTONS(self->log_bucket_id);
    tmp_writer.current += sizeof(uint16_t);
    char *records_count_p = tmp_writer.current; // Pointer to the records count. Will be filled in later.
    tmp_writer.current += sizeof(uint16_t);

    ssize_t remaining_size = self->log_properties.max_log_bucket_size < (tmp_writer.end - tmp_writer.current)
            ? self->log_properties.max_log_bucket_size
            : tmp_writer.end - tmp_writer.current;
    KAA_LOG_TRACE(self->logger, KAA_ERR_NONE, "Extracting log records... (remaining bucket size is %zu)", remaining_size);

    uint16_t records_count = 0;

    while (!error) {
        size_t record_len = 0;
        error = ext_log_storage_write_next_record(self->log_storage
                , tmp_writer.current + sizeof(uint32_t), remaining_size - sizeof(uint32_t)
                , self->log_bucket_id, &record_len);
        switch (error) {
        case KAA_ERR_NONE:
            ++records_count;
            *((uint32_t *) tmp_writer.current) = KAA_HTONL(record_len);
            tmp_writer.current += (sizeof(uint32_t) + record_len);
            kaa_platform_message_write_alignment(&tmp_writer);
            remaining_size -= (kaa_aligned_size_get(record_len) + sizeof(uint32_t));
            break;
        case KAA_ERR_NOT_FOUND:
        case KAA_ERR_INSUFFICIENT_BUFFER:
            // These errors are normal if they appear after at least one record got serialized
            if (!records_count) {
                KAA_LOG_ERROR(self->logger, error, "Failed to write the log record");
                return error;
            }
            break;
        default:
            KAA_LOG_ERROR(self->logger, error, "Failed to write the log record");
            return error;
        }
    }

    size_t total_size = tmp_writer.current - writer->current - KAA_EXTENSION_HEADER_SIZE;
    KAA_LOG_TRACE(self->logger, KAA_ERR_NONE, "Extracted %u log records; total extension size %lu", records_count, total_size);

    *((uint32_t *) extension_size_p) = KAA_HTONL(total_size);
    *((uint16_t *) records_count_p) = KAA_HTONS(records_count);
    *writer = tmp_writer;

    return KAA_ERR_NONE;
}



kaa_error_t kaa_logging_handle_server_sync(kaa_log_collector_t *self
                                         , kaa_platform_message_reader_t *reader
                                         , uint32_t extension_options
                                         , size_t extension_length)
{
    KAA_RETURN_IF_NIL2(self, reader, KAA_ERR_BADPARAM);

    KAA_LOG_INFO(self->logger, KAA_ERR_NONE, "Received log server sync");

    if (extension_length >= sizeof(uint32_t)) {
        uint16_t id = KAA_NTOHS(*((uint16_t *) reader->current));
        reader->current += sizeof(uint16_t);
        logging_sync_result_t result = *((const uint8_t *) reader->current);
        reader->current += sizeof(uint16_t);

        KAA_LOG_DEBUG(self->logger, KAA_ERR_NONE, "Log bucket with ID %u : %s"
                , id
                , (result == LOGGING_RESULT_SUCCESS ? "uploaded successfully." : "upload failed.")
            );

        if (result == LOGGING_RESULT_SUCCESS) {
            ext_log_storage_remove_by_bucket_id(self->log_storage, id);
        } else {
            ext_log_storage_unmark_by_bucket_id(self->log_storage, id);
        }
        update_storage(self);
    }
    return KAA_ERR_NONE;

}

#endif

