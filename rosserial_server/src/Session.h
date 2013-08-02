#include <map>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/function.hpp>

#include <ros/ros.h>
#include <rosserial_msgs/TopicInfo.h>
#include <topic_tools/shape_shifter.h>
#include <std_msgs/Time.h>

#include "AsyncReadBuffer.h"
#include "topic_handlers.h"

#define INT16_MAX 0x7fff


template<typename Socket>
class Session
{
public:
  Session(boost::asio::io_service& io_service)
    : socket_(io_service), client_version(PROTOCOL_UNKNOWN),
      timeout_interval_(boost::posix_time::milliseconds(5000)),
      attempt_interval_(boost::posix_time::milliseconds(1000)),
      sync_timer_(io_service), 
      buffer_(socket_, buffer_max, 
              boost::bind(&Session::read_failed, this,
                          boost::asio::placeholders::error))
  {
    callbacks_[rosserial_msgs::TopicInfo::ID_PUBLISHER]
        = boost::bind(&Session::setup_publisher, this, _1);
    callbacks_[rosserial_msgs::TopicInfo::ID_SUBSCRIBER]
        = boost::bind(&Session::setup_subscriber, this, _1);
    callbacks_[rosserial_msgs::TopicInfo::ID_TIME]
        = boost::bind(&Session::handle_time, this, _1);
  }

  virtual ~Session()
  {
    ROS_INFO("Ending session.");  
  }

  Socket& socket()
  {
    return socket_;
  }

  void start()
  {
    ROS_INFO("Starting session.");

    attempt_sync();
    read_sync_header();
  }

  enum Version {
    PROTOCOL_UNKNOWN = 0,
    PROTOCOL_VER1 = 1,
    PROTOCOL_VER2 = 2
  };

private:
  //// RECEIVING MESSAGES ////
  // TODO: Total message timeout, implement primarily in ReadBuffer.

  void read_sync_header() {
    buffer_.read(1, boost::bind(&Session::read_sync_first, this, _1));
  }
  
  void read_sync_first(ros::serialization::IStream& stream) {
    uint8_t sync;
    stream >> sync;
    if (sync == 0xff) {
      buffer_.read(1, boost::bind(&Session::read_sync_second, this, _1));
    } else {
      read_sync_header();
    }
  }
  
  void read_sync_second(ros::serialization::IStream& stream) {
    uint8_t sync;
    stream >> sync;
    if (client_version == PROTOCOL_UNKNOWN) {
      if (sync == 0xff) {
        ROS_WARN("Attached client is using protocol VER1 (groovy)");
        client_version = PROTOCOL_VER1;
      } else if (sync == 0xfe) {
        ROS_INFO("Attached client is using protocol VER2 (hydro)");
        client_version = PROTOCOL_VER2;
      }
    }
    if (sync == 0xff && client_version == PROTOCOL_VER1) {
      buffer_.read(4, boost::bind(&Session::read_id_length, this, _1));
    } else if (sync == 0xfe && client_version == PROTOCOL_VER2) {
      buffer_.read(5, boost::bind(&Session::read_id_length, this, _1)); 
    } else {
      read_sync_header();
    }
  }

  void read_id_length(ros::serialization::IStream& stream) {
    uint16_t topic_id, length;
    uint8_t length_checksum;
    stream >> topic_id >> length;
    if (client_version == PROTOCOL_VER2) {
      // Protocol VER2 introduced a new checksum byte for the message length.
      stream >> length_checksum;
      uint8_t length_checksum_computed = 255 - (((length & 255) + (length >> 8)) % 256);
      if (length_checksum != length_checksum_computed) {
        ROS_WARN("Bad message header length checksum. Dropping message from client.");
        read_sync_header();
        return;
      }
    }
    ROS_DEBUG("Received message header with length %d and topic_id=%d", length, topic_id);

    if (length > INT16_MAX) {
        ROS_WARN("Declared length is %d, in excess of maximum %d. Dropping message from client.", length, INT16_MAX);
        read_sync_header();
        return;
    }

    // Read message length + checksum byte.
    buffer_.read(length + 1, boost::bind(&Session::read_body, this,
                                         _1, topic_id));
  }

  void read_body(ros::serialization::IStream& stream, uint16_t topic_id) {
    ROS_DEBUG("Received body of length %d for message on topic %d.", stream.getLength(), topic_id);
    
    ros::serialization::IStream checksum_stream(stream.getData(), stream.getLength());
    if (message_checksum(checksum_stream, topic_id) != 0xff) {
      ROS_WARN("Received message on topicId=%d, length=%d with bad checksum.", topic_id, stream.getLength());
    } else {
      if (callbacks_.count(topic_id) == 1) {
        try {
          callbacks_[topic_id](stream);
        } catch(ros::serialization::StreamOverrunException e) {
          if (topic_id < 100) {
            ROS_ERROR("Buffer overrun when attempting to parse setup message.");
            ROS_ERROR_ONCE("Is this firmware from a pre-Groovy rosserial?");
          } else {
            ROS_WARN("Buffer overrun when attempting to parse user message.");
          }
        }
      } else {
        ROS_WARN("Received message with unrecognized topicId (%d).", topic_id);
        // TODO: Resynchronize on multiples?
      }
    }

    // Kickoff next message read.
    read_sync_header();
  }

  void read_failed(const boost::system::error_code& error) {
    if (error == boost::system::errc::no_buffer_space) {
      // No worries. Begin syncing on a new message.
      ROS_WARN("Overrun on receive buffer. Attempting to regain rx sync.");
      read_sync_header();
    } else if (error) {
      // When some other read error has occurred, delete the whole session, which destroys
      // all publishers and subscribers.
      socket_.cancel();
      ROS_DEBUG_STREAM("Socket asio error: " << error);
      ROS_WARN("Stopping session due to read error.");
      delete this;
    }
  }

  //// SENDING MESSAGES ////

  void write_message(std::vector<uint8_t>& message,
                     const uint16_t topic_id, 
                     Session::Version version) {
    uint8_t overhead_bytes = 0;
    switch(version) {
      case PROTOCOL_VER1: overhead_bytes = 7; break;
      case PROTOCOL_VER2: overhead_bytes = 8; break;
      default:
        ROS_WARN("Aborting write_message: protocol unspecified.");
    }

    uint16_t length = overhead_bytes + message.size();
    boost::shared_ptr< std::vector<uint8_t> > buffer_ptr(new std::vector<uint8_t>(length));

    ros::serialization::IStream checksum_stream(
        message.size() > 0 ? &message[0] : NULL, message.size());
    uint8_t checksum = message_checksum(checksum_stream, topic_id);

    ros::serialization::OStream stream(&buffer_ptr->at(0), buffer_ptr->size()); 
    stream << (uint16_t)0xffff << topic_id << (uint16_t)message.size();
    memcpy(stream.advance(message.size()), &message[0], message.size());
    stream << checksum;

    // Will call immediately if we are already on the io_service thread. Otherwise,
    // the request is queued up and executed on that thread.
    socket_.get_io_service().dispatch(
        boost::bind(&Session::write_buffer, this, buffer_ptr));
  }

  // Function which is dispatched onto the io_service thread by write_message, so that 
  // write_message may be safely called directly from the ROS background spinning thread.
  void write_buffer(boost::shared_ptr< std::vector<uint8_t> > buffer_ptr) {
    boost::asio::async_write(socket_, boost::asio::buffer(*buffer_ptr),
          boost::bind(&Session::write_cb, this, boost::asio::placeholders::error, buffer_ptr));
  }

  void write_cb(const boost::system::error_code& error,
                boost::shared_ptr< std::vector<uint8_t> > buffer_ptr) { 
    if (error) {
      ROS_DEBUG_STREAM("Socket asio error: " << error);
      ROS_WARN("Stopping session due to write error.");
      delete this;
    }
    // Buffer is destructed when this function exits.
  }

  //// SYNC WATCHDOG ////
  void attempt_sync() {
    request_topics();
    set_sync_timeout(attempt_interval_);
  }

  void set_sync_timeout(const boost::posix_time::time_duration& interval) {
    sync_timer_.cancel();
    sync_timer_.expires_from_now(interval);
    sync_timer_.async_wait(boost::bind(&Session::sync_timeout, this,
          boost::asio::placeholders::error));
  }

  void sync_timeout(const boost::system::error_code& error) {
    if (error == boost::asio::error::operation_aborted) {
      return;
    }
    ROS_WARN("Sync with device lost.");
    attempt_sync(); 
  }

  //// HELPERS ////
  void request_topics() {
    ROS_DEBUG("Sending request topics message for VER1 protocol.");
    std::vector<uint8_t> message(0);
    write_message(message, rosserial_msgs::TopicInfo::ID_PUBLISHER, PROTOCOL_VER1);
  }

  static uint8_t message_checksum(ros::serialization::IStream& stream, const uint16_t topic_id) {
    uint8_t sum = (topic_id >> 8) + topic_id + (stream.getLength() >> 8) + stream.getLength();
    for (uint16_t i = 0; i < stream.getLength(); ++i) {
      sum += stream.getData()[i];
    }
    return 255 - sum;
  }

  //// RECEIVED MESSAGE HANDLERS ////

  /**
   * 
   */
  void setup_publisher(ros::serialization::IStream& stream) {
    rosserial_msgs::TopicInfo topic_info;
    ros::serialization::Serializer<rosserial_msgs::TopicInfo>::read(stream, topic_info);

    boost::shared_ptr<Publisher> pub(new Publisher(nh_, topic_info));
    publishers_[topic_info.topic_id] = pub;
    callbacks_[topic_info.topic_id] = boost::bind(&Publisher::handle, pub, _1);

    set_sync_timeout(timeout_interval_);
  }
  
  void setup_subscriber(ros::serialization::IStream& stream) {
    rosserial_msgs::TopicInfo topic_info;
    ros::serialization::Serializer<rosserial_msgs::TopicInfo>::read(stream, topic_info);

    boost::shared_ptr<Subscriber> sub(new Subscriber(nh_, topic_info,
        boost::bind(&Session::write_message, this, _1, topic_info.topic_id, client_version)));
    subscribers_[topic_info.topic_id] = sub;

    set_sync_timeout(timeout_interval_);
  }

  void handle_time(ros::serialization::IStream& stream) {
    std_msgs::Time time;
    time.data = ros::Time::now();

    size_t length = ros::serialization::serializationLength(time);
    std::vector<uint8_t> message(length);

    ros::serialization::OStream ostream(&message[0], length);
    ros::serialization::Serializer<std_msgs::Time>::write(ostream, time); 
 
    write_message(message, rosserial_msgs::TopicInfo::ID_TIME, client_version);

    // The MCU requesting the time from the server is the sync notification. This
    // call moves the timeout forward.
    set_sync_timeout(timeout_interval_);
  }

  Socket socket_;
  AsyncReadBuffer<Socket> buffer_;
  enum { buffer_max = 1023 };
  ros::NodeHandle nh_;

  Session::Version client_version;
  boost::posix_time::time_duration timeout_interval_;
  boost::posix_time::time_duration attempt_interval_;
  boost::asio::deadline_timer sync_timer_;

  std::map< uint16_t, boost::function<void(ros::serialization::IStream)> > callbacks_;
  std::map< uint16_t, boost::shared_ptr<Publisher> > publishers_;
  std::map< uint16_t, boost::shared_ptr<Subscriber> > subscribers_;
};

