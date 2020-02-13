/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdio>
#include <thread>

#include <fbzmq/service/monitor/ZmqMonitor.h>
#include <fbzmq/zmq/Context.h>
#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

#include <openr/common/Constants.h>
#include <openr/common/OpenrClient.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/link-monitor/tests/MockNetlinkSystemHandler.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/tests/OpenrThriftServerWrapper.h>

using namespace openr;

class OpenrCtrlFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // Create zmq-monitor
    zmqMonitor = std::make_unique<fbzmq::ZmqMonitor>(
        monitorSubmitUrl_, "inproc://monitor_pub_url", context_);
    zmqMonitorThread_ = std::thread([&]() { zmqMonitor->run(); });

    // Create PersistentStore
    std::string const configStoreFile = "/tmp/openr-ctrl-handler-test.bin";
    // start fresh
    std::remove(configStoreFile.data());
    persistentStore = std::make_unique<PersistentStore>(
        nodeName,
        configStoreFile,
        context_,
        Constants::kPersistentStoreInitialBackoff,
        Constants::kPersistentStoreMaxBackoff,
        true /* dryrun */);
    persistentStoreThread_ = std::thread([&]() { persistentStore->run(); });

    // Create KvStore module
    kvStoreWrapper = std::make_unique<KvStoreWrapper>(
        context_,
        nodeName,
        std::chrono::seconds(1),
        std::chrono::seconds(1),
        std::unordered_map<std::string, thrift::PeerSpec>(),
        std::nullopt,
        std::nullopt,
        std::chrono::milliseconds(1),
        true /* enableFloodOptimization */,
        true /* isFloodRoot */,
        std::unordered_set<std::string>{
            thrift::KvStore_constants::kDefaultArea(), "plane", "pod"});
    kvStoreWrapper->run();

    // Create Decision module
    decision = std::make_shared<Decision>(
        nodeName, /* node name */
        true, /* enable v4 */
        true, /* computeLfaPaths */
        false, /* enableOrderedFib */
        false, /* bgpDryRun */
        false, /* bgpUseIgpMetric */
        AdjacencyDbMarker{"adj:"},
        PrefixDbMarker{"prefix:"},
        std::chrono::milliseconds(10),
        std::chrono::milliseconds(500),
        folly::none,
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        routeUpdatesQueue_,
        monitorSubmitUrl_,
        context_);
    decisionThread_ = std::thread([&]() { decision->run(); });

    // Create Fib module
    fib = std::make_shared<Fib>(
        nodeName,
        -1, /* thrift port */
        true, /* dryrun */
        true, /* enableSegmentRouting */
        false, /* enableOrderedFib */
        std::chrono::seconds(2),
        false, /* waitOnDecision */
        routeUpdatesQueue_.getReader(),
        interfaceUpdatesQueue_.getReader(),
        MonitorSubmitUrl{"inproc://monitor-sub"},
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        context_);
    fibThread_ = std::thread([&]() { fib->run(); });

    // Create PrefixManager module
    prefixManager = std::make_shared<PrefixManager>(
        nodeName,
        persistentStore.get(),
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        monitorSubmitUrl_,
        PrefixDbMarker{Constants::kPrefixDbMarker.str()},
        false /* create per prefix keys */,
        false,
        std::chrono::seconds(0),
        Constants::kKvStoreDbTtl,
        context_);
    prefixManagerThread_ = std::thread([&]() { prefixManager->run(); });

    // Create MockNetlinkSystemHandler
    mockNlHandler =
        std::make_shared<MockNetlinkSystemHandler>(context_, platformPubUrl_);
    systemServer = std::make_shared<apache::thrift::ThriftServer>();
    systemServer->setNumIOWorkerThreads(1);
    systemServer->setNumAcceptThreads(1);
    systemServer->setPort(0);
    systemServer->setInterface(mockNlHandler);
    systemThriftThread.start(systemServer);

    // Create LinkMonitor
    re2::RE2::Options regexOpts;
    std::string regexErr;
    auto includeRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    includeRegexList->Add("po.*", &regexErr);
    includeRegexList->Compile();

    linkMonitor = std::make_shared<LinkMonitor>(
        context_,
        nodeName,
        systemThriftThread.getAddress()->getPort(),
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::move(includeRegexList),
        nullptr,
        nullptr, // redistribute interface name
        std::vector<thrift::IpPrefix>{},
        false /* useRttMetric */,
        false /* enable perf measurement */,
        true /* enable v4 */,
        true /* enable segment routing */,
        false /* prefix type mpls */,
        false /* prefix fwd algo KSP2_ED_ECMP */,
        AdjacencyDbMarker{Constants::kAdjDbMarker.str()},
        interfaceUpdatesQueue_,
        neighborUpdatesQueue_.getReader(),
        monitorSubmitUrl_,
        persistentStore.get(),
        false,
        PrefixManagerLocalCmdUrl{prefixManager->inprocCmdUrl},
        platformPubUrl_,
        std::chrono::seconds(1),
        // link flap backoffs, set low to keep UT runtime low
        std::chrono::milliseconds(1),
        std::chrono::milliseconds(8),
        Constants::kKvStoreDbTtl);
    linkMonitorThread_ = std::thread([&]() { linkMonitor->run(); });

    // spin up an openrThriftServer
    openrThriftServerWrapper_ = std::make_shared<OpenrThriftServerWrapper>(
        nodeName,
        monitorSubmitUrl_,
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        context_);

    // add module into thriftServer module map
    openrThriftServerWrapper_->addModuleType(
        thrift::OpenrModuleType::PERSISTENT_STORE, persistentStore);
    openrThriftServerWrapper_->addModuleType(
        thrift::OpenrModuleType::KVSTORE, kvStoreWrapper->getKvStore());
    openrThriftServerWrapper_->addModuleType(
        thrift::OpenrModuleType::DECISION, decision);
    openrThriftServerWrapper_->addModuleType(thrift::OpenrModuleType::FIB, fib);
    openrThriftServerWrapper_->addModuleType(
        thrift::OpenrModuleType::PREFIX_MANAGER, prefixManager);
    openrThriftServerWrapper_->addModuleType(
        thrift::OpenrModuleType::LINK_MONITOR, linkMonitor);

    // start running openrThriftServer
    openrThriftServerWrapper_->run();

    // initialize openrCtrlClient talking to server
    openrCtrlThriftClient_ =
        getOpenrCtrlPlainTextClient<apache::thrift::HeaderClientChannel>(
            evb_,
            folly::IPAddress("::1"),
            openrThriftServerWrapper_->getOpenrCtrlThriftPort());
  }

  void
  TearDown() override {
    routeUpdatesQueue_.close();
    interfaceUpdatesQueue_.close();
    neighborUpdatesQueue_.close();

    openrThriftServerWrapper_->stop();

    linkMonitor->stop();
    linkMonitorThread_.join();

    persistentStore->stop();
    persistentStoreThread_.join();

    prefixManager->stop();
    prefixManagerThread_.join();

    mockNlHandler->stop();
    systemThriftThread.stop();

    fib->stop();
    fibThread_.join();

    decision->stop();
    decisionThread_.join();

    kvStoreWrapper->stop();

    zmqMonitor->stop();
    zmqMonitorThread_.join();
  }

  thrift::PeerSpec
  createPeerSpec(const std::string& pubUrl, const std::string& cmdUrl) {
    thrift::PeerSpec peerSpec;
    peerSpec.pubUrl = pubUrl;
    peerSpec.cmdUrl = cmdUrl;
    return peerSpec;
  }

  thrift::PrefixEntry
  createPrefixEntry(const std::string& prefix, thrift::PrefixType prefixType) {
    thrift::PrefixEntry prefixEntry;
    prefixEntry.prefix = toIpPrefix(prefix);
    prefixEntry.type = prefixType;
    return prefixEntry;
  }

 private:
  const MonitorSubmitUrl monitorSubmitUrl_{"inproc://monitor-submit-url"};
  const PlatformPublisherUrl platformPubUrl_{"inproc://platform-pub-url"};

  messaging::ReplicateQueue<thrift::RouteDatabaseDelta> routeUpdatesQueue_;
  messaging::ReplicateQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue_;
  messaging::ReplicateQueue<thrift::SparkNeighborEvent> neighborUpdatesQueue_;

  fbzmq::Context context_;
  folly::EventBase evb_;

  std::thread zmqMonitorThread_;
  std::thread decisionThread_;
  std::thread fibThread_;
  std::thread prefixManagerThread_;
  std::thread persistentStoreThread_;
  std::thread linkMonitorThread_;

  std::unique_ptr<fbzmq::ZmqMonitor> zmqMonitor;
  std::shared_ptr<Decision> decision;
  std::shared_ptr<Fib> fib;
  std::shared_ptr<PrefixManager> prefixManager;
  std::shared_ptr<PersistentStore> persistentStore;
  std::shared_ptr<LinkMonitor> linkMonitor;

  apache::thrift::util::ScopedServerThread systemThriftThread;
  std::shared_ptr<apache::thrift::ThriftServer> systemServer;

 public:
  const std::string nodeName{"thanos@universe"};
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper;
  std::shared_ptr<MockNetlinkSystemHandler> mockNlHandler;
  std::shared_ptr<OpenrThriftServerWrapper> openrThriftServerWrapper_{nullptr};
  std::unique_ptr<openr::thrift::OpenrCtrlCppAsyncClient>
      openrCtrlThriftClient_{nullptr};
};

TEST_F(OpenrCtrlFixture, getMyNodeName) {
  std::string res = "";
  openrCtrlThriftClient_->sync_getMyNodeName(res);
  EXPECT_EQ(nodeName, res);
}

TEST_F(OpenrCtrlFixture, PrefixManagerApis) {
  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("10.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("11.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("20.0.0.0/8", thrift::PrefixType::BGP),
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    openrCtrlThriftClient_->sync_advertisePrefixes(
        std::vector<thrift::PrefixEntry>{std::move(prefixes)});
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    openrCtrlThriftClient_->sync_withdrawPrefixes(
        std::vector<thrift::PrefixEntry>{std::move(prefixes)});
    openrCtrlThriftClient_->sync_withdrawPrefixesByType(
        thrift::PrefixType::LOOPBACK);
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    openrCtrlThriftClient_->sync_syncPrefixesByType(
        thrift::PrefixType::BGP,
        std::vector<thrift::PrefixEntry>{std::move(prefixes)});
  }

  {
    const std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    std::vector<thrift::PrefixEntry> res;
    openrCtrlThriftClient_->sync_getPrefixes(res);
    EXPECT_EQ(prefixes, res);
  }

  {
    std::vector<thrift::PrefixEntry> res;
    openrCtrlThriftClient_->sync_getPrefixesByType(
        res, thrift::PrefixType::LOOPBACK);
    EXPECT_EQ(0, res.size());
  }
}

TEST_F(OpenrCtrlFixture, RouteApis) {
  {
    thrift::RouteDatabase db;
    openrCtrlThriftClient_->sync_getRouteDb(db);
    EXPECT_EQ(nodeName, db.thisNodeName);
    EXPECT_EQ(0, db.unicastRoutes.size());
    EXPECT_EQ(0, db.mplsRoutes.size());
  }

  {
    thrift::RouteDatabase db;
    openrCtrlThriftClient_->sync_getRouteDbComputed(db, nodeName);
    EXPECT_EQ(nodeName, db.thisNodeName);
    EXPECT_EQ(0, db.unicastRoutes.size());
    EXPECT_EQ(0, db.mplsRoutes.size());
  }

  {
    const std::string testNode("avengers@universe");
    thrift::RouteDatabase db;
    openrCtrlThriftClient_->sync_getRouteDbComputed(db, testNode);
    EXPECT_EQ(testNode, db.thisNodeName);
    EXPECT_EQ(0, db.unicastRoutes.size());
    EXPECT_EQ(0, db.mplsRoutes.size());
  }

  {
    std::vector<thrift::UnicastRoute> filterRet;
    std::vector<std::string> prefixes{"10.46.2.0", "10.46.2.0/24"};
    openrCtrlThriftClient_->sync_getUnicastRoutesFiltered(filterRet, prefixes);
    EXPECT_EQ(0, filterRet.size());
  }

  {
    std::vector<thrift::UnicastRoute> allRouteRet;
    openrCtrlThriftClient_->sync_getUnicastRoutes(allRouteRet);
    EXPECT_EQ(0, allRouteRet.size());
  }
  {
    std::vector<thrift::MplsRoute> filterRet;
    std::vector<std::int32_t> labels{1, 2};
    openrCtrlThriftClient_->sync_getMplsRoutesFiltered(filterRet, labels);
    EXPECT_EQ(0, filterRet.size());
  }
  {
    std::vector<thrift::MplsRoute> allRouteRet;
    openrCtrlThriftClient_->sync_getMplsRoutes(allRouteRet);
    EXPECT_EQ(0, allRouteRet.size());
  }
}

TEST_F(OpenrCtrlFixture, PerfApis) {
  thrift::PerfDatabase db;
  openrCtrlThriftClient_->sync_getPerfDb(db);
  EXPECT_EQ(nodeName, db.thisNodeName);
}

TEST_F(OpenrCtrlFixture, DecisionApis) {
  {
    thrift::AdjDbs db;
    openrCtrlThriftClient_->sync_getDecisionAdjacencyDbs(db);
    EXPECT_EQ(0, db.size());
  }

  {
    thrift::PrefixDbs db;
    openrCtrlThriftClient_->sync_getDecisionPrefixDbs(db);
    EXPECT_EQ(0, db.size());
  }
}

TEST_F(OpenrCtrlFixture, KvStoreApis) {
  thrift::KeyVals keyVals;
  keyVals["key1"] = createThriftValue(1, "node1", std::string("value1"));
  keyVals["key11"] = createThriftValue(1, "node1", std::string("value11"));
  keyVals["key111"] = createThriftValue(1, "node1", std::string("value111"));
  keyVals["key2"] = createThriftValue(1, "node1", std::string("value2"));
  keyVals["key22"] = createThriftValue(1, "node1", std::string("value22"));
  keyVals["key222"] = createThriftValue(1, "node1", std::string("value222"));
  keyVals["key3"] = createThriftValue(1, "node3", std::string("value3"));
  keyVals["key33"] = createThriftValue(1, "node33", std::string("value33"));
  keyVals["key333"] = createThriftValue(1, "node33", std::string("value333"));

  thrift::KeyVals keyValsPod;
  keyValsPod["keyPod1"] =
      createThriftValue(1, "node1", std::string("valuePod1"));
  keyValsPod["keyPod2"] =
      createThriftValue(1, "node1", std::string("valuePod2"));

  thrift::KeyVals keyValsPlane;
  keyValsPlane["keyPlane1"] =
      createThriftValue(1, "node1", std::string("valuePlane1"));
  keyValsPlane["keyPlane2"] =
      createThriftValue(1, "node1", std::string("valuePlane2"));
  //
  // area list get
  //
  {
    thrift::AreasConfig area;
    openrCtrlThriftClient_->sync_getAreasConfig(area);
    EXPECT_EQ(3, area.areas.size());
    EXPECT_EQ(area.areas.count("plane"), 1);
    EXPECT_EQ(area.areas.count("pod"), 1);
    EXPECT_EQ(area.areas.count(thrift::KvStore_constants::kDefaultArea()), 1);
    EXPECT_EQ(area.areas.count("none"), 0);
  }

  //
  // Key set/get
  //

  {
    thrift::KeySetParams setParams;
    setParams.keyVals = keyVals;

    openrCtrlThriftClient_->sync_setKvStoreKeyVals(
        setParams, thrift::KvStore_constants::kDefaultArea());

    setParams.solicitResponse = false;
    openrCtrlThriftClient_->sync_setKvStoreKeyVals(
        setParams, thrift::KvStore_constants::kDefaultArea());

    thrift::KeySetParams setParamsPod;
    setParamsPod.keyVals = keyValsPod;
    openrCtrlThriftClient_->sync_setKvStoreKeyVals(setParamsPod, "pod");

    thrift::KeySetParams setParamsPlane;
    setParamsPlane.keyVals = keyValsPlane;
    openrCtrlThriftClient_->sync_setKvStoreKeyVals(setParamsPlane, "plane");
  }

  {
    std::vector<std::string> filterKeys{"key11", "key2"};
    thrift::Publication pub;
    openrCtrlThriftClient_->sync_getKvStoreKeyVals(pub, filterKeys);
    EXPECT_EQ(2, pub.keyVals.size());
    EXPECT_EQ(keyVals.at("key2"), pub.keyVals["key2"]);
    EXPECT_EQ(keyVals.at("key11"), pub.keyVals["key11"]);
  }
  // pod keys
  {
    std::vector<std::string> filterKeys{"keyPod1"};
    thrift::Publication pub;
    openrCtrlThriftClient_->sync_getKvStoreKeyValsArea(pub, filterKeys, "pod");
    EXPECT_EQ(1, pub.keyVals.size());
    EXPECT_EQ(keyValsPod.at("keyPod1"), pub.keyVals["keyPod1"]);
  }

  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    params.prefix = "key3";
    params.originatorIds.insert("node3");

    openrCtrlThriftClient_->sync_getKvStoreKeyValsFiltered(pub, params);
    EXPECT_EQ(3, pub.keyVals.size());
    EXPECT_EQ(keyVals.at("key3"), pub.keyVals["key3"]);
    EXPECT_EQ(keyVals.at("key33"), pub.keyVals["key33"]);
    EXPECT_EQ(keyVals.at("key333"), pub.keyVals["key333"]);
  }
  // with areas
  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    params.prefix = "keyP";
    params.originatorIds.insert("node1");

    openrCtrlThriftClient_->sync_getKvStoreKeyValsFilteredArea(
        pub, params, "plane");
    EXPECT_EQ(2, pub.keyVals.size());
    EXPECT_EQ(keyValsPlane.at("keyPlane1"), pub.keyVals["keyPlane1"]);
    EXPECT_EQ(keyValsPlane.at("keyPlane2"), pub.keyVals["keyPlane2"]);
  }

  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    params.prefix = "key3";
    params.originatorIds.insert("node3");

    openrCtrlThriftClient_->sync_getKvStoreHashFiltered(pub, params);
    EXPECT_EQ(3, pub.keyVals.size());
    auto value3 = keyVals.at("key3");
    value3.value = folly::none;
    auto value33 = keyVals.at("key33");
    value33.value = folly::none;
    auto value333 = keyVals.at("key333");
    value333.value = folly::none;
    EXPECT_EQ(value3, pub.keyVals["key3"]);
    EXPECT_EQ(value33, pub.keyVals["key33"]);
    EXPECT_EQ(value333, pub.keyVals["key333"]);
  }

  //
  // Dual and Flooding APIs
  //

  {
    thrift::DualMessages messages;
    openrCtrlThriftClient_->sync_processKvStoreDualMessage(
        messages, thrift::KvStore_constants::kDefaultArea());
  }

  {
    thrift::FloodTopoSetParams params;
    params.rootId = nodeName;
    openrCtrlThriftClient_->sync_updateFloodTopologyChild(
        params, thrift::KvStore_constants::kDefaultArea());
  }

  {
    thrift::SptInfos ret;
    openrCtrlThriftClient_->sync_getSpanningTreeInfos(
        ret, thrift::KvStore_constants::kDefaultArea());
    EXPECT_EQ(1, ret.infos.size());
    ASSERT_NE(ret.infos.end(), ret.infos.find(nodeName));
    EXPECT_EQ(0, ret.counters.neighborCounters.size());
    EXPECT_EQ(1, ret.counters.rootCounters.size());
    EXPECT_EQ(nodeName, ret.floodRootId);
    EXPECT_EQ(0, ret.floodPeers.size());

    thrift::SptInfo sptInfo = ret.infos.at(nodeName);
    EXPECT_EQ(0, sptInfo.cost);
    ASSERT_TRUE(sptInfo.parent.hasValue());
    EXPECT_EQ(nodeName, sptInfo.parent.value());
    EXPECT_EQ(0, sptInfo.children.size());
  }

  //
  // Peers APIs
  //

  const thrift::PeersMap peers{
      {"peer1", createPeerSpec("inproc:://peer1-pub", "inproc://peer1-cmd")},
      {"peer2", createPeerSpec("inproc:://peer2-pub", "inproc://peer2-cmd")},
      {"peer3", createPeerSpec("inproc:://peer3-pub", "inproc://peer3-cmd")}};

  // do the same with non-default area
  const thrift::PeersMap peersPod{
      {"peer11", createPeerSpec("inproc:://peer11-pub", "inproc://peer11-cmd")},
      {"peer21", createPeerSpec("inproc:://peer21-pub", "inproc://peer21-cmd")},
  };

  {
    openrCtrlThriftClient_->sync_addUpdateKvStorePeers(
        peers, thrift::KvStore_constants::kDefaultArea());

    openrCtrlThriftClient_->sync_addUpdateKvStorePeers(peersPod, "pod");

    thrift::PeersMap ret;
    openrCtrlThriftClient_->sync_getKvStorePeersArea(
        ret, thrift::KvStore_constants::kDefaultArea());

    EXPECT_EQ(3, ret.size());
    EXPECT_EQ(peers.at("peer1"), ret.at("peer1"));
    EXPECT_EQ(peers.at("peer2"), ret.at("peer2"));
    EXPECT_EQ(peers.at("peer3"), ret.at("peer3"));
  }

  {
    std::vector<std::string> peersToDel{"peer2"};
    openrCtrlThriftClient_->sync_deleteKvStorePeers(
        peersToDel, thrift::KvStore_constants::kDefaultArea());

    thrift::PeersMap ret;
    openrCtrlThriftClient_->sync_getKvStorePeersArea(
        ret, thrift::KvStore_constants::kDefaultArea());
    EXPECT_EQ(2, ret.size());
    EXPECT_EQ(peers.at("peer1"), ret.at("peer1"));
    EXPECT_EQ(peers.at("peer3"), ret.at("peer3"));
  }

  {
    thrift::PeersMap ret;
    openrCtrlThriftClient_->sync_getKvStorePeersArea(ret, "pod");

    EXPECT_EQ(2, ret.size());
    EXPECT_EQ(peersPod.at("peer11"), ret.at("peer11"));
    EXPECT_EQ(peersPod.at("peer21"), ret.at("peer21"));
  }

  {
    std::vector<std::string> peersToDel{"peer21"};
    openrCtrlThriftClient_->sync_deleteKvStorePeers(peersToDel, "pod");

    thrift::PeersMap ret;
    openrCtrlThriftClient_->sync_getKvStorePeersArea(ret, "pod");
    EXPECT_EQ(1, ret.size());
    EXPECT_EQ(peersPod.at("peer11"), ret.at("peer11"));
    EXPECT_EQ(ret.count("peer21"), 0);
  }

  //
  // Subscribe API
  //

  {
    std::atomic<int> received{0};
    const std::string key{"snoop-key"};
    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto subscription =
        handler->subscribeKvStore().toClientStream().subscribeExTry(
            folly::getEventBase(), [&received, key](auto&& t) {
              // Consider publication only if `key` is present
              // NOTE: There can be updates to prefix or adj keys
              if (!t.hasValue() or not t->keyVals.count(key)) {
                return;
              }
              auto& pub = *t;
              EXPECT_EQ(1, pub.keyVals.size());
              ASSERT_EQ(1, pub.keyVals.count(key));
              EXPECT_EQ("value1", pub.keyVals.at(key).value.value());
              EXPECT_EQ(received + 1, pub.keyVals.at(key).version);
              received++;
            });
    EXPECT_EQ(1, handler->getNumKvStorePublishers());
    kvStoreWrapper->setKey(
        key, createThriftValue(1, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(1, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(2, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(3, "node1", std::string("value1")));

    // Check we should receive-3 updates
    while (received < 3) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }

  //
  // Subscribe and Get API
  //

  {
    std::atomic<int> received{0};
    const std::string key{"snoop-key"};
    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler->semifuture_subscribeAndGetKvStore().get();

    // Expect 10 keys in the initial dump
    // NOTE: there may be extra keys from PrefixManager & LinkMonitor)
    EXPECT_LE(10, responseAndSubscription.response.keyVals.size());
    ASSERT_EQ(1, responseAndSubscription.response.keyVals.count(key));
    EXPECT_EQ(
        responseAndSubscription.response.keyVals.at(key),
        createThriftValue(3, "node1", std::string("value1")));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStream()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              // Consider publication only if `key` is present
              // NOTE: There can be updates to prefix or adj keys
              if (!t.hasValue() or not t->keyVals.count(key)) {
                return;
              }
              auto& pub = *t;
              EXPECT_EQ(1, pub.keyVals.size());
              ASSERT_EQ(1, pub.keyVals.count(key));
              EXPECT_EQ("value1", pub.keyVals.at(key).value.value());
              EXPECT_EQ(received + 4, pub.keyVals.at(key).version);
              received++;
            });
    EXPECT_EQ(1, handler->getNumKvStorePublishers());
    kvStoreWrapper->setKey(
        key, createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(5, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(6, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key,
        createThriftValue(7, "node1", std::string("value1")),
        folly::none,
        "pod");
    kvStoreWrapper->setKey(
        key,
        createThriftValue(8, "node1", std::string("value1")),
        folly::none,
        "plane");

    // Check we should receive-3 updates
    while (received < 5) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }
}

TEST_F(OpenrCtrlFixture, LinkMonitorApis) {
  // create an interface
  mockNlHandler->sendLinkEvent("po1011", 100, true);
  const std::string ifName = "po1011";
  const std::string adjName = "night@king";

  {
    openrCtrlThriftClient_->sync_setNodeOverload();
    openrCtrlThriftClient_->sync_unsetNodeOverload();
  }

  {
    openrCtrlThriftClient_->sync_setInterfaceOverload(ifName);
    openrCtrlThriftClient_->sync_unsetInterfaceOverload(ifName);
  }

  {
    openrCtrlThriftClient_->sync_setInterfaceMetric(ifName, 110);
    openrCtrlThriftClient_->sync_unsetInterfaceMetric(ifName);
  }

  {
    openrCtrlThriftClient_->sync_setAdjacencyMetric(ifName, adjName, 110);
    openrCtrlThriftClient_->sync_unsetAdjacencyMetric(ifName, adjName);
  }

  {
    thrift::DumpLinksReply reply;
    openrCtrlThriftClient_->sync_getInterfaces(reply);
    EXPECT_EQ(nodeName, reply.thisNodeName);
    EXPECT_FALSE(reply.isOverloaded);
    EXPECT_EQ(1, reply.interfaceDetails.size());
  }

  {
    thrift::OpenrVersions ret;
    openrCtrlThriftClient_->sync_getOpenrVersion(ret);
    EXPECT_LE(ret.lowestSupportedVersion, ret.version);
  }

  {
    thrift::BuildInfo info;
    openrCtrlThriftClient_->sync_getBuildInfo(info);
    EXPECT_NE("", info.buildMode);
  }

  {
    thrift::AdjacencyDatabase adjDb;
    openrCtrlThriftClient_->sync_getLinkMonitorAdjacencies(adjDb);
    EXPECT_EQ(0, adjDb.adjacencies.size());
  }
}

TEST_F(OpenrCtrlFixture, PersistentStoreApis) {
  {
    const std::string key = "key1";
    const std::string value = "value1";
    openrCtrlThriftClient_->sync_setConfigKey(key, value);
  }

  {
    const std::string key = "key2";
    const std::string value = "value2";
    openrCtrlThriftClient_->sync_setConfigKey(key, value);
  }

  {
    const std::string key = "key1";
    openrCtrlThriftClient_->sync_eraseConfigKey(key);
  }

  {
    const std::string key = "key2";
    std::string ret = "";
    openrCtrlThriftClient_->sync_getConfigKey(ret, key);
    EXPECT_EQ("value2", ret);
  }

  {
    const std::string key = "key1";
    std::string ret = "";
    EXPECT_THROW(
        openrCtrlThriftClient_->sync_getConfigKey(ret, key),
        thrift::OpenrError);
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
