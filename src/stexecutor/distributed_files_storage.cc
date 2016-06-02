// Copyright 2016 The "Stakhanov" project authors. All rights reserved.
// Use of this source code is governed by a GPLv2 license that can be
// found in the LICENSE file.

#include "stexecutor/distributed_files_storage.h"

#include "boost/asio/ip/tcp.hpp"
#include "boost/asio/read.hpp"
#include "boost/asio/read_until.hpp"
#include "boost/asio/streambuf.hpp"
#include "boost/asio/write.hpp"
#include "boost/property_tree/ptree.hpp"
#include "log4cplus/logger.h"
#include "log4cplus/loggingmacros.h"
#include "stexecutor/redis_client_pool.h"
#include "stexecutor/redis_key_prefixes.h"
#include "third_party/redisclient/src/redisclient/redissyncclient.h"

namespace {

log4cplus::Logger logger_ = log4cplus::Logger::getRoot();

std::string GetLocalHostName() {
  // MSDN says 256 bytes should be always enough.
  char buffer[257];
  if (gethostname(buffer, sizeof(buffer)) != 0) {
    DWORD error = WSAGetLastError();
    LOG4CPLUS_ERROR(logger_, "Failed get local host name, error " << error);
    return std::string();
  }
  return std::string(buffer);
}

std::string StorageIdToRedisKey(const std::string& storage_id) {
  return redis_key_prefixes::kStoredFileHosts + storage_id;
}

}  // namespace

DistributedFilesStorage::DistributedFilesStorage(
    const boost::property_tree::ptree& config,
    const std::shared_ptr<RedisClientPool>& redis_client_pool)
    : FilesystemFilesStorage(config),
      http_port_(0),
      redis_client_pool_(redis_client_pool) {
  LoadConfig(config);
  this_host_name_ = GetLocalHostName();
  LOG4CPLUS_INFO(logger_, "This host name " << this_host_name_.c_str());
}

void DistributedFilesStorage::OnStorageIdFilled(
    const std::string& storage_id) {
  std::unique_ptr<RedisSyncClient> redis_client =
      redis_client_pool_->GetClient();
  redis_client->command(
      "RPUSH",
      StorageIdToRedisKey(storage_id),
      this_host_name_);
  redis_client_pool_->ReturnClient(std::move(redis_client));
}

bool DistributedFilesStorage::OnRequestedMissedStorageId(
    const std::string& storage_id) {
  boost::filesystem::path dest_path = PreparePlace(storage_id);
  if (dest_path.empty())
    return false;
  std::string host_name = GetHostNameForStorageId(storage_id);
  if (host_name.empty()) {
    LOG4CPLUS_WARN(
        logger_, "Failed get host name for storage id " << storage_id.c_str());
    return false;
  }
  boost::filesystem::path temp_path = PrepareTempPath();
  if (!DownloadFile(host_name, storage_id, temp_path)) {
    LOG4CPLUS_WARN(
        logger_, "Failed download file " << storage_id.c_str());
    return false;
  }
  // Check that we downloaded proper content. We don't want some
  // files storage breakage to propagate to entire cluster.
  std::string downloaded_id = GetFileHash(temp_path);
  if (downloaded_id != storage_id) {
    LOG4CPLUS_WARN(
        logger_,
        "Downloaded file storage id mismatch. Requred id " <<
            storage_id.c_str());
    return false;
  }

  if (!MoveTempFile(temp_path, dest_path)) {
    LOG4CPLUS_WARN(
        logger_, "Failed download file " << storage_id.c_str());
    return false;
  }
  OnStorageIdFilled(storage_id);
  return true;
}

std::string DistributedFilesStorage::GetHostNameForStorageId(
    const std::string& storage_id) {
  std::unique_ptr<RedisSyncClient> redis_client =
      redis_client_pool_->GetClient();
  std::string redis_key = StorageIdToRedisKey(storage_id);
  RedisValue len_val = redis_client->command("LLEN", redis_key);
  int list_len = len_val.toInt();
  if (list_len == 0) {
    LOG4CPLUS_INFO(logger_, "No hosts for storage id " << storage_id.c_str());
    redis_client_pool_->ReturnClient(std::move(redis_client));
    return std::string();
  }
  // Use random host number as most primitive way of distributing load.
  int host_index = std::rand() % list_len;
  std::ostringstream host_index_buf;
  host_index_buf << host_index;
  RedisValue host_val = redis_client->command(
      "LINDEX", redis_key, host_index_buf.str());
  redis_client_pool_->ReturnClient(std::move(redis_client));
  if (!host_val.isOk()) {
    LOG4CPLUS_INFO(
        logger_, "Failed get host name for storage id " << storage_id.c_str());
    return std::string();
  }
  return host_val.toString();
}

bool DistributedFilesStorage::DownloadFile(
    const std::string& host_name,
    const std::string& storage_id,
    const boost::filesystem::path& dest_path) {
  const std::string rel_file_path = RelFilePathFromId(storage_id);
  if (rel_file_path.empty())
    return false;
  boost::system::error_code error;
  boost::asio::ip::tcp::resolver resolver(io_service_);
  boost::asio::ip::tcp::resolver::query query(host_name, "http");
  boost::asio::ip::tcp::resolver::iterator endpoint_iterator =
      resolver.resolve(query, error);
  if (error) {
    LOG4CPLUS_ERROR(
        logger_,
        "Failed resolve host " << host_name.c_str() << " error " << error);
    return false;
  }
  boost::asio::ip::tcp::endpoint dest_endpoint = *endpoint_iterator;
  dest_endpoint.port(http_port_);
  boost::asio::ip::tcp::socket socket(io_service_);
  socket.connect(dest_endpoint, error);
  if (error) {
    LOG4CPLUS_ERROR(
        logger_,
        "Failed connect to host " << host_name.c_str() << " error " << error);
    return false;
  }

  boost::asio::streambuf request;
  std::ostream request_stream(&request);
  request_stream << "GET /" << rel_file_path << " HTTP/1.0\r\n";
  request_stream << "Host: " << host_name << "\r\n";
  request_stream << "Accept: */*\r\n";
  request_stream << "Connection: close\r\n\r\n";

  // Send the request.
  boost::asio::write(socket, request);

  // Read the response status line. The response streambuf will automatically
  // grow to accommodate the entire line. The growth may be limited by passing
  // a maximum size to the streambuf constructor.
  boost::asio::streambuf response;
  boost::asio::read_until(socket, response, "\r\n");

  // Check that response is OK.
  std::istream response_stream(&response);
  std::string http_version;
  response_stream >> http_version;
  int status_code = 0;
  response_stream >> status_code;
  std::string status_message;
  std::getline(response_stream, status_message);
  if (!response_stream || http_version.substr(0, 5) != "HTTP/") {
    LOG4CPLUS_ERROR(logger_, "Invalid response");
    return false;
  }
  if (status_code != 200) {
    LOG4CPLUS_ERROR(logger_,
                    "Response to " << host_name.c_str() <<
                    " returned with status code " << status_code);
    return false;
  }

  // Read the response headers, which are terminated by a blank line.
  boost::asio::read_until(socket, response, "\r\n\r\n");

  // Process the response headers.
  std::string header;
  while (std::getline(response_stream, header) && header != "\r") {
  }

  boost::filesystem::ofstream output_stream;
  output_stream.open(dest_path, std::ios::out | std::ios::binary);
  if (!output_stream.is_open()) {
    return false;
  }
  // Write whatever content we already have to output.
  if (response.size() > 0)
    output_stream << &response;

  // Read until EOF, writing data to output as we go.
  while (boost::asio::read(socket, response,
        boost::asio::transfer_at_least(1), error)) {
    output_stream << &response;
  }
  if (error != boost::asio::error::eof) {
    LOG4CPLUS_ERROR(logger_, "Failed receive file, error " << error);
    return false;
  }
  return true;
}

void DistributedFilesStorage::LoadConfig(
    const boost::property_tree::ptree& config) {
  http_port_ = config.get<int>("distributed_files_storage.port");
}
