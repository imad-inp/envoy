#include "envoy/api/v2/cds.pb.h"
#include "envoy/api/v2/discovery.pb.h"
#include "envoy/grpc/status.h"
#include "envoy/stats/scope.h"

#include "common/config/protobuf_link_hacks.h"
#include "common/config/resources.h"
#include "common/protobuf/protobuf.h"
#include "common/protobuf/utility.h"

#include "test/common/grpc/grpc_client_integration.h"
#include "test/integration/http_integration.h"
#include "test/integration/utility.h"
#include "test/mocks/server/mocks.h"
#include "test/test_common/network_utility.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "absl/synchronization/notification.h"
#include "gtest/gtest.h"

using testing::AssertionFailure;
using testing::AssertionResult;
using testing::AssertionSuccess;
using testing::IsSubstring;

namespace Envoy {
namespace {

// TODO(fredlas) Move to test/config/utility.cc once there are other xDS tests that use gRPC.
const char Config[] = R"EOF(
admin:
  access_log_path: /dev/null
  address:
    socket_address:
      address: 127.0.0.1
      port_value: 0
dynamic_resources:
  cds_config:
    api_config_source:
      api_type: DELTA_GRPC
      grpc_services:
        envoy_grpc:
          cluster_name: my_cds_cluster
static_resources:
  clusters:
  - name: my_cds_cluster
    http2_protocol_options: {}
    hosts:
      socket_address:
        address: 127.0.0.1
        port_value: 0
  listeners:
    name: http
    address:
      socket_address:
        address: 127.0.0.1
        port_value: 0
    filter_chains:
      filters:
        name: envoy.http_connection_manager
        config:
          stat_prefix: config_test
          http_filters:
            name: envoy.router
          codec_type: HTTP2
          route_config:
            name: route_config_0
            validate_clusters: false
            virtual_hosts:
              name: integration
              routes:
              - route:
                  cluster: cluster_1
                match:
                  prefix: "/cluster1"
              - route:
                  cluster: cluster_2
                match:
                  prefix: "/cluster2"
              domains: "*"
)EOF";
const char ClusterName1[] = "cluster_1";
const char ClusterName2[] = "cluster_2";
const int UpstreamIndex1 = 1;
const int UpstreamIndex2 = 2;

class DeltaCdsIntegrationTest : public HttpIntegrationTest,
                                public Grpc::GrpcClientIntegrationParamTest {
public:
  DeltaCdsIntegrationTest()
      : HttpIntegrationTest(Http::CodecClient::Type::HTTP2, ipVersion(), realTime(), Config) {}

  void TearDown() override {
    cleanUpXdsConnection();
    test_server_.reset();
    fake_upstreams_.clear();
  }

  // TODO(fredlas) Move to test/config/utility.cc once there are other xDS tests that use gRPC.
  envoy::api::v2::Cluster buildCluster(const std::string& name, int upstream_index) {
    return TestUtility::parseYaml<envoy::api::v2::Cluster>(
        fmt::format(R"EOF(
      name: {}
      connect_timeout: 5s
      type: STATIC
      load_assignment:
        cluster_name: {}
        endpoints:
        - lb_endpoints:
          - endpoint:
              address:
                socket_address:
                  address: {}
                  port_value: {}
      lb_policy: ROUND_ROBIN
      http2_protocol_options: {{}}
    )EOF",
                    name, name, Network::Test::getLoopbackAddressString(ipVersion()),
                    fake_upstreams_[upstream_index]->localAddress()->ip()->port()));
  }

  // Overridden to insert this stuff into the initialize() at the very beginning of
  // HttpIntegrationTest::testRouterRequestAndResponseWithBody().
  void initialize() override {
    // Controls how many fake_upstreams_.emplace_back(new FakeUpstream) will happen in
    // BaseIntegrationTest::createUpstreams() (which is part of initialize()).
    // Make sure this number matches the size of the 'clusters' repeated field in the bootstrap
    // config that you use!
    setUpstreamCount(1);                                  // the CDS cluster
    setUpstreamProtocol(FakeHttpConnection::Type::HTTP2); // CDS uses gRPC uses HTTP2.

    // BaseIntegrationTest::initialize() does many things:
    // 1) It appends to fake_upstreams_ as many as you asked for via setUpstreamCount().
    // 2) It updates your bootstrap config with the ports your fake upstreams are actually listening
    //    on (since you're supposed to leave them as 0).
    // 3) It creates and starts an IntegrationTestServer - the thing that wraps the almost-actual
    //    Envoy used in the tests.
    // 4) Bringing up the server usually entails waiting to ensure that any listeners specified in
    //    the bootstrap config have come up, and registering them in a port map (see lookupPort()).
    //    However, this test needs to defer all of that to later.
    defer_listener_finalization_ = true;
    HttpIntegrationTest::initialize();

    // Create the regular (i.e. not an xDS server) upstream. We create it manually here after
    // initialize() because finalize() expects all fake_upstreams_ to correspond to a static
    // cluster in the bootstrap config - which we don't want since we're testing dynamic CDS!
    fake_upstreams_.emplace_back(new FakeUpstream(0, FakeHttpConnection::Type::HTTP2, version_,
                                                  timeSystem(), enable_half_close_));
    fake_upstreams_[UpstreamIndex1]->set_allow_unexpected_disconnects(false);

    // Now that the upstream has been created, process Envoy's request to discover it.
    // (First, we have to let Envoy establish its connection to the CDS server.)
    acceptXdsConnection();

    EXPECT_TRUE(compareDeltaDiscoveryRequest(Config::TypeUrl::get().Cluster, {}, {}));
    sendDeltaDiscoveryResponse<envoy::api::v2::Cluster>(
        {buildCluster(ClusterName1, UpstreamIndex1)}, {}, "55");
    // We can continue the test once we're sure that Envoy's ClusterManager has made use of
    // the DiscoveryResponse describing cluster_1 that we sent.
    // 2 because the statically specified CDS server itself counts as a cluster.
    test_server_->waitForGaugeGe("cluster_manager.active_clusters", 2);

    // Wait for our statically specified listener to become ready, and register its port in the
    // test framework's downstream listener port map.
    test_server_->waitUntilListenersReady();
    registerTestServerPorts({"http"});
  }

  void acceptXdsConnection() {
    AssertionResult result = // xds_connection_ is filled with the new FakeHttpConnection.
        fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, xds_connection_);
    RELEASE_ASSERT(result, result.message());
    result = xds_connection_->waitForNewStream(*dispatcher_, xds_stream_);
    RELEASE_ASSERT(result, result.message());
    xds_stream_->startGrpcStream();
    fake_upstreams_[0]->set_allow_unexpected_disconnects(true);
  }
};

INSTANTIATE_TEST_CASE_P(IpVersionsClientType, DeltaCdsIntegrationTest,
                        GRPC_CLIENT_INTEGRATION_PARAMS);

// 1) Envoy starts up with no static clusters (other than the CDS-over-gRPC server).
// 2) Envoy is told of a cluster via CDS.
// 3) We send Envoy a request, which we verify is properly proxied to and served by that cluster.
// 4) Envoy is told that cluster is gone.
// 5) We send Envoy a request, which should 503.
// 6) Envoy is told that the cluster is back.
// 7) We send Envoy a request, which we verify is properly proxied to and served by that cluster.
TEST_P(DeltaCdsIntegrationTest, CdsClusterUpDownUp) {
  // Calls our initialize(), which includes establishing a listener, route, and cluster.
  testRouterHeaderOnlyRequestAndResponse(nullptr, UpstreamIndex1, "/cluster1");

  // Tell Envoy that cluster_1 is gone.
  EXPECT_TRUE(compareDeltaDiscoveryRequest(Config::TypeUrl::get().Cluster, {}, {}));
  sendDeltaDiscoveryResponse<envoy::api::v2::Cluster>({}, {ClusterName1}, "42");
  // We can continue the test once we're sure that Envoy's ClusterManager has made use of
  // the DiscoveryResponse that says cluster_1 is gone.
  test_server_->waitForCounterGe("cluster_manager.cluster_removed", 1);

  // Now that cluster_1 is gone, the listener (with its routing to cluster_1) should 503.
  BufferingStreamDecoderPtr response = IntegrationUtil::makeSingleRequest(
      lookupPort("http"), "GET", "/cluster1", "", downstream_protocol_, version_, "foo.com");
  ASSERT_TRUE(response->complete());
  EXPECT_STREQ("503", response->headers().Status()->value().c_str());

  cleanupUpstreamAndDownstream();
  codec_client_->waitForDisconnect();

  // Tell Envoy that cluster_1 is back.
  EXPECT_TRUE(compareDeltaDiscoveryRequest(Config::TypeUrl::get().Cluster, {}, {}));
  sendDeltaDiscoveryResponse<envoy::api::v2::Cluster>({buildCluster(ClusterName1, UpstreamIndex1)},
                                                      {}, "413");

  // We can continue the test once we're sure that Envoy's ClusterManager has made use of
  // the DiscoveryResponse describing cluster_1 that we sent. Again, 2 includes CDS server.
  test_server_->waitForGaugeGe("cluster_manager.active_clusters", 2);

  // Does *not* call our initialize().
  testRouterHeaderOnlyRequestAndResponse(nullptr, UpstreamIndex1, "/cluster1");

  cleanupUpstreamAndDownstream();
}

// Tests adding a cluster, adding another, then removing the first.
TEST_P(DeltaCdsIntegrationTest, TwoClusters) {
  // Calls our initialize(), which includes establishing a listener, route, and cluster.
  testRouterHeaderOnlyRequestAndResponse(nullptr, UpstreamIndex1, "/cluster1");
  cleanupUpstreamAndDownstream();
  codec_client_->waitForDisconnect();

  // Add another fake upstream, to be cluster_2.
  fake_upstreams_.emplace_back(new FakeUpstream(0, FakeHttpConnection::Type::HTTP2, version_,
                                                timeSystem(), enable_half_close_));
  fake_upstreams_[UpstreamIndex2]->set_allow_unexpected_disconnects(false);

  // Tell Envoy that cluster_2 is here.
  EXPECT_TRUE(compareDeltaDiscoveryRequest(Config::TypeUrl::get().Cluster, {}, {}));
  sendDeltaDiscoveryResponse<envoy::api::v2::Cluster>({buildCluster(ClusterName2, UpstreamIndex2)},
                                                      {}, "42");
  // The '3' includes the fake CDS server.
  test_server_->waitForGaugeGe("cluster_manager.active_clusters", 3);

  // A request for cluster_2 should be fine.
  testRouterHeaderOnlyRequestAndResponse(nullptr, UpstreamIndex2, "/cluster2");
  cleanupUpstreamAndDownstream();
  codec_client_->waitForDisconnect();

  // Tell Envoy that cluster_1 is gone.
  EXPECT_TRUE(compareDeltaDiscoveryRequest(Config::TypeUrl::get().Cluster, {}, {}));
  sendDeltaDiscoveryResponse<envoy::api::v2::Cluster>({}, {ClusterName1}, "42");
  // We can continue the test once we're sure that Envoy's ClusterManager has made use of
  // the DiscoveryResponse that says cluster_1 is gone.
  test_server_->waitForCounterGe("cluster_manager.cluster_removed", 1);

  // Even with cluster_1 is gone, a request for cluster_2 should be fine.
  testRouterHeaderOnlyRequestAndResponse(nullptr, UpstreamIndex2, "/cluster2");
  cleanupUpstreamAndDownstream();
  codec_client_->waitForDisconnect();

  // Tell Envoy that cluster_1 is back.
  EXPECT_TRUE(compareDeltaDiscoveryRequest(Config::TypeUrl::get().Cluster, {}, {}));
  sendDeltaDiscoveryResponse<envoy::api::v2::Cluster>({buildCluster(ClusterName1, UpstreamIndex1)},
                                                      {}, "413");

  // We can continue the test once we're sure that Envoy's ClusterManager has made use of
  // the DiscoveryResponse describing cluster_1 that we sent. Again, 3 includes CDS server.
  test_server_->waitForGaugeGe("cluster_manager.active_clusters", 3);

  // Does *not* call our initialize().
  testRouterHeaderOnlyRequestAndResponse(nullptr, UpstreamIndex1, "/cluster1");

  cleanupUpstreamAndDownstream();
}

// Tests that when Envoy's xDS gRPC stream dis/reconnects, Envoy can inform the server of the
// resources it already has: the reconnected stream need not start with a state-of-the-world update.
TEST_P(DeltaCdsIntegrationTest, VersionsRememberedAfterReconnect) {
  // Calls our initialize(), which includes establishing a listener, route, and cluster.
  testRouterHeaderOnlyRequestAndResponse(nullptr, UpstreamIndex1, "/cluster1");
  cleanupUpstreamAndDownstream();
  codec_client_->waitForDisconnect();

  // Close the connection carrying Envoy's xDS gRPC stream...
  AssertionResult result = xds_connection_->close();
  RELEASE_ASSERT(result, result.message());
  result = xds_connection_->waitForDisconnect();
  RELEASE_ASSERT(result, result.message());
  xds_connection_.reset();
  // ...and reconnect it.
  acceptXdsConnection();

  // Upon reconnecting, the Envoy should tell us its current resource versions.
  envoy::api::v2::DeltaDiscoveryRequest request;
  result = xds_stream_->waitForGrpcMessage(*dispatcher_, request);
  RELEASE_ASSERT(result, result.message());
  const auto& initial_resource_versions = request.initial_resource_versions();
  EXPECT_EQ("55", initial_resource_versions.at(std::string(ClusterName1)));
  EXPECT_EQ(1, initial_resource_versions.size());

  // Add another fake upstream, to be cluster_2.
  fake_upstreams_.emplace_back(new FakeUpstream(0, FakeHttpConnection::Type::HTTP2, version_,
                                                timeSystem(), enable_half_close_));
  fake_upstreams_[UpstreamIndex2]->set_allow_unexpected_disconnects(false);
  // Tell Envoy that cluster_2 is here. This update does *not* need to include cluster_1,
  // which Envoy should already know about despite the disconnect.
  sendDeltaDiscoveryResponse<envoy::api::v2::Cluster>({buildCluster(ClusterName2, UpstreamIndex2)},
                                                      {}, "42");
  // The '3' includes the fake CDS server.
  test_server_->waitForGaugeGe("cluster_manager.active_clusters", 3);

  // A request for cluster_1 should be fine.
  testRouterHeaderOnlyRequestAndResponse(nullptr, UpstreamIndex1, "/cluster1");
  cleanupUpstreamAndDownstream();
  codec_client_->waitForDisconnect();
  // A request for cluster_2 should be fine.
  testRouterHeaderOnlyRequestAndResponse(nullptr, UpstreamIndex2, "/cluster2");
  cleanupUpstreamAndDownstream();
  codec_client_->waitForDisconnect();
}

} // namespace
} // namespace Envoy
