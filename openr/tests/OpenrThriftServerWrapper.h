/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/zmq/Zmq.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>
#include <thread>

namespace openr {

class OpenrThriftServerWrapper {
 private:
  OpenrEventBase evb_;
  std::thread evbThread_;
  std::shared_ptr<OpenrCtrlHandler> openrCtrlHandler_{nullptr};
  apache::thrift::util::ScopedServerThread openrCtrlThriftServerThread_;
  std::string const nodeName_;
  MonitorSubmitUrl const monitorSubmitUrl_;
  fbzmq::Context& context_;

 public:
  OpenrThriftServerWrapper(
      std::string const& nodeName,
      Decision* decision,
      Fib* fib,
      KvStore* kvStore,
      LinkMonitor* linkMonitor,
      PersistentStore* configStore,
      PrefixManager* prefixManager,
      std::shared_ptr<const Config> config,
      MonitorSubmitUrl const& monitorSubmitUrl,
      fbzmq::Context& context);

  // start Open/R thrift server
  void run();

  // stop Open/R thrift server
  void stop();

  inline uint16_t
  getOpenrCtrlThriftPort() {
    return openrCtrlThriftServerThread_.getAddress()->getPort();
  }

  inline std::shared_ptr<OpenrCtrlHandler>&
  getOpenrCtrlHandler() {
    return openrCtrlHandler_;
  }

  // Pointers to Open/R modules
  Decision* decision_{nullptr};
  Fib* fib_{nullptr};
  KvStore* kvStore_{nullptr};
  LinkMonitor* linkMonitor_{nullptr};
  PersistentStore* configStore_{nullptr};
  PrefixManager* prefixManager_{nullptr};
  std::shared_ptr<const Config> config_;
};

} // namespace openr
