// Copyright 2014-2017 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <limits>

#include "rmw/error_handling.h"
#include "rmw/impl/cpp/macros.hpp"
#include "rmw/types.h"

#include "rmw_connext_shared_cpp/types.hpp"

#include "rmw_connext_cpp/connext_static_subscriber_info.hpp"
#include "rmw_connext_cpp/identifier.hpp"

#include "rosidl_typesupport_connext_cpp/connext_static_cdr_stream.hpp"

// include patched generated code from the build folder
#include "./connext_static_serialized_dataSupport.h"
#include "./connext_static_serialized_data.h"

static bool
take(
  DDSDataReader * dds_data_reader,
  bool ignore_local_publications,
  ConnextStaticCDRStream * cdr_stream,
  bool * taken,
  void * sending_publication_handle)
{
  if (!dds_data_reader) {
    RMW_SET_ERROR_MSG("dds_data_reader is null");
    return false;
  }
  if (!cdr_stream) {
    RMW_SET_ERROR_MSG("cdr stream handle is null");
    return false;
  }
  if (!taken) {
    RMW_SET_ERROR_MSG("taken handle is null");
    return false;
  }

  ConnextStaticSerializedDataDataReader * data_reader =
    ConnextStaticSerializedDataDataReader::narrow(dds_data_reader);
  if (!data_reader) {
    RMW_SET_ERROR_MSG("failed to narrow data reader");
    return false;
  }

  ConnextStaticSerializedDataSeq dds_messages;
  DDS_SampleInfoSeq sample_infos;
  bool ignore_sample = false;

  DDS_ReturnCode_t status = data_reader->take(
    dds_messages,
    sample_infos,
    1,
    DDS_ANY_SAMPLE_STATE,
    DDS_ANY_VIEW_STATE,
    DDS_ANY_INSTANCE_STATE);
  if (status == DDS_RETCODE_NO_DATA) {
    data_reader->return_loan(dds_messages, sample_infos);
    *taken = false;
    return true;
  }
  if (status != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("take failed");
    data_reader->return_loan(dds_messages, sample_infos);
  }

  DDS_SampleInfo & sample_info = sample_infos[0];
  if (!sample_info.valid_data) {
    // skip sample without data
    ignore_sample = true;
  } else if (ignore_local_publications) {
    // compare the lower 12 octets of the guids from the sender and this receiver
    // if they are equal the sample has been sent from this process and should be ignored
    DDS_GUID_t sender_guid = sample_info.original_publication_virtual_guid;
    DDS_InstanceHandle_t receiver_instance_handle = dds_data_reader->get_instance_handle();
    ignore_sample = true;
    for (size_t i = 0; i < 12; ++i) {
      DDS_Octet * sender_element = &(sender_guid.value[i]);
      DDS_Octet * receiver_element = &(reinterpret_cast<DDS_Octet *>(&receiver_instance_handle)[i]);
      if (*sender_element != *receiver_element) {
        ignore_sample = false;
        break;
      }
    }
  }
  if (sample_info.valid_data && sending_publication_handle) {
    *static_cast<DDS_InstanceHandle_t *>(sending_publication_handle) =
      sample_info.publication_handle;
  }

  if (!ignore_sample) {
    cdr_stream->buffer_length = dds_messages[0].serialized_data.length();
    // TODO(karsten1987): This malloc has to go!
    cdr_stream->buffer =
      reinterpret_cast<char *>(malloc(cdr_stream->buffer_length * sizeof(char)));

    if (cdr_stream->buffer_length > (std::numeric_limits<unsigned int>::max)()) {
      RMW_SET_ERROR_MSG("cdr_stream->buffer_length unexpectedly larger than max unsiged int value");
      data_reader->return_loan(dds_messages, sample_infos);
      *taken = false;
      return DDS_RETCODE_ERROR;
    }
    for (unsigned int i = 0; i < static_cast<unsigned int>(cdr_stream->buffer_length); ++i) {
      cdr_stream->buffer[i] = dds_messages[0].serialized_data[i];
    }
    *taken = true;
  } else {
    *taken = false;
  }

  data_reader->return_loan(dds_messages, sample_infos);

  return status == DDS_RETCODE_OK;
}

extern "C"
{
rmw_ret_t
_take(
  const rmw_subscription_t * subscription,
  void * ros_message,
  bool * taken,
  DDS_InstanceHandle_t * sending_publication_handle)
{
  if (!subscription) {
    RMW_SET_ERROR_MSG("subscription handle is null");
    return RMW_RET_ERROR;
  }
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription handle,
    subscription->implementation_identifier, rti_connext_identifier,
    return RMW_RET_ERROR)

  if (!ros_message) {
    RMW_SET_ERROR_MSG("ros message handle is null");
    return RMW_RET_ERROR;
  }
  if (!taken) {
    RMW_SET_ERROR_MSG("taken handle is null");
    return RMW_RET_ERROR;
  }

  ConnextStaticSubscriberInfo * subscriber_info =
    static_cast<ConnextStaticSubscriberInfo *>(subscription->data);
  if (!subscriber_info) {
    RMW_SET_ERROR_MSG("subscriber info handle is null");
    return RMW_RET_ERROR;
  }
  DDSDataReader * topic_reader = subscriber_info->topic_reader_;
  if (!topic_reader) {
    RMW_SET_ERROR_MSG("topic reader handle is null");
    return RMW_RET_ERROR;
  }
  const message_type_support_callbacks_t * callbacks = subscriber_info->callbacks_;
  if (!callbacks) {
    RMW_SET_ERROR_MSG("callbacks handle is null");
    return RMW_RET_ERROR;
  }

  // fetch the incoming message as cdr stream
  ConnextStaticCDRStream cdr_stream;
  if (!take(
      topic_reader, subscriber_info->ignore_local_publications, &cdr_stream, taken,
      sending_publication_handle))
  {
    RMW_SET_ERROR_MSG("error occured while taking message");
    return RMW_RET_ERROR;
  }
  // convert the cdr stream to the message
  if (!callbacks->to_message(&cdr_stream, ros_message)) {
    RMW_SET_ERROR_MSG("can't convert cdr stream to ros message");
    return RMW_RET_ERROR;
  }

  // the call to take allocates memory for the serialized message
  // we have to free this here again
  free(cdr_stream.buffer);

  return RMW_RET_OK;
}

rmw_ret_t
rmw_take(const rmw_subscription_t * subscription, void * ros_message, bool * taken)
{
  return _take(subscription, ros_message, taken, nullptr);
}

rmw_ret_t
rmw_take_with_info(
  const rmw_subscription_t * subscription,
  void * ros_message,
  bool * taken,
  rmw_message_info_t * message_info)
{
  if (!message_info) {
    RMW_SET_ERROR_MSG("message info is null");
    return RMW_RET_ERROR;
  }
  DDS_InstanceHandle_t sending_publication_handle;
  auto ret = _take(subscription, ros_message, taken, &sending_publication_handle);
  if (ret != RMW_RET_OK) {
    // Error string is already set.
    return RMW_RET_ERROR;
  }

  rmw_gid_t * sender_gid = &message_info->publisher_gid;
  sender_gid->implementation_identifier = rti_connext_identifier;
  memset(sender_gid->data, 0, RMW_GID_STORAGE_SIZE);
  auto detail = reinterpret_cast<ConnextPublisherGID *>(sender_gid->data);
  detail->publication_handle = sending_publication_handle;

  return RMW_RET_OK;
}

rmw_ret_t
_take_serialized_message(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message,
  bool * taken,
  DDS_InstanceHandle_t * sending_publication_handle)
{
  if (!subscription) {
    RMW_SET_ERROR_MSG("subscription handle is null");
    return RMW_RET_ERROR;
  }
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription handle,
    subscription->implementation_identifier, rti_connext_identifier,
    return RMW_RET_ERROR)

  if (!serialized_message) {
    RMW_SET_ERROR_MSG("ros message handle is null");
    return RMW_RET_ERROR;
  }
  if (!taken) {
    RMW_SET_ERROR_MSG("taken handle is null");
    return RMW_RET_ERROR;
  }

  ConnextStaticSubscriberInfo * subscriber_info =
    static_cast<ConnextStaticSubscriberInfo *>(subscription->data);
  if (!subscriber_info) {
    RMW_SET_ERROR_MSG("subscriber info handle is null");
    return RMW_RET_ERROR;
  }
  DDSDataReader * topic_reader = subscriber_info->topic_reader_;
  if (!topic_reader) {
    RMW_SET_ERROR_MSG("topic reader handle is null");
    return RMW_RET_ERROR;
  }
  const message_type_support_callbacks_t * callbacks = subscriber_info->callbacks_;
  if (!callbacks) {
    RMW_SET_ERROR_MSG("callbacks handle is null");
    return RMW_RET_ERROR;
  }

  // fetch the incoming message as cdr stream
  ConnextStaticCDRStream cdr_stream;
  if (!take(
      topic_reader, subscriber_info->ignore_local_publications, &cdr_stream, taken,
      sending_publication_handle))
  {
    RMW_SET_ERROR_MSG("error occured while taking message");
    return RMW_RET_ERROR;
  }

  serialized_message->buffer_length = cdr_stream.buffer_length;
  // we don't free the allocated memory
  // and thus can directly set the pointer
  serialized_message->buffer = cdr_stream.buffer;

  return RMW_RET_OK;
}

rmw_ret_t
rmw_take_serialized_message(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message,
  bool * taken)
{
  return _take_serialized_message(subscription, serialized_message, taken, nullptr);
}

rmw_ret_t
rmw_take_serialized_message_with_info(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message,
  bool * taken,
  rmw_message_info_t * message_info)
{
  if (!message_info) {
    RMW_SET_ERROR_MSG("message info is null");
    return RMW_RET_ERROR;
  }
  DDS_InstanceHandle_t sending_publication_handle;
  auto ret =
    _take_serialized_message(subscription, serialized_message, taken, &sending_publication_handle);
  if (ret != RMW_RET_OK) {
    // Error string is already set.
    return RMW_RET_ERROR;
  }

  rmw_gid_t * sender_gid = &message_info->publisher_gid;
  sender_gid->implementation_identifier = rti_connext_identifier;
  memset(sender_gid->data, 0, RMW_GID_STORAGE_SIZE);
  auto detail = reinterpret_cast<ConnextPublisherGID *>(sender_gid->data);
  detail->publication_handle = sending_publication_handle;

  return RMW_RET_OK;
}
}  // extern "C"