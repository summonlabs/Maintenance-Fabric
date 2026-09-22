// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/config/text.hpp"
#include "mf/core/bytes.hpp"
#include "mf/core/checked.hpp"
#include "mf/core/hash.hpp"
#include "mf/core/id.hpp"
#include "mf/core/limits.hpp"
#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "tests/mf_test.hpp"

using namespace mf;

MF_TEST(ids, strong_identity_is_not_interchangeable) {
  const JobId job = JobId::from_u64(7);
  const EvidenceId evidence = EvidenceId::from_u64(7);
  MF_CHECK_EQ(job.value(), evidence.value());
  MF_CHECK_EQ(to_string(job), std::string("job:7"));
  MF_CHECK_EQ(to_string(evidence), std::string("ev:7"));
  MF_CHECK(job.valid());
  MF_CHECK(!JobId{}.valid());
  MF_CHECK_EQ(JobId::from_u64(7).next().value(), 8u);
}

MF_TEST(ids, parse_round_trip_and_rejections) {
  const std::optional<JobId> job = parse_id<JobTag>("job:42");
  MF_CHECK(job.has_value());
  MF_CHECK_EQ(job->value(), 42u);
  MF_CHECK(!parse_id<JobTag>("job:").has_value());
  MF_CHECK(!parse_id<JobTag>("job:-1").has_value());
  MF_CHECK(!parse_id<JobTag>("ev:1").has_value());
  MF_CHECK(!parse_id<JobTag>("job:99999999999999999999").has_value());
  MF_CHECK(!parse_id<JobTag>("job").has_value());

  AttemptId attempt;
  attempt.job = JobId::from_u64(3);
  attempt.generation = GenerationId::from_u64(2);
  attempt.ordinal = AttemptOrdinal::from_u64(5);
  MF_CHECK_EQ(to_string(attempt), std::string("job:3/gen:2/att:5"));
  const std::optional<AttemptId> parsed = parse_attempt_id("job:3/gen:2/att:5");
  MF_CHECK(parsed.has_value());
  MF_CHECK_EQ(*parsed, attempt);
  MF_CHECK(!parse_attempt_id("job:3/gen:2").has_value());
  MF_CHECK(!parse_attempt_id("job:3/gen:2/att:5/extra:1").has_value());
}

MF_TEST(identity, target_and_domain_ids) {
  const std::optional<TargetId> link = parse_target_id("link:12");
  MF_CHECK(link.has_value());
  MF_CHECK(link->kind == TargetKind::Link);
  MF_CHECK_EQ(link->index, 12u);
  MF_CHECK(!parse_target_id("link:0x10").has_value());
  MF_CHECK(!parse_target_id("widget:1").has_value());
  const std::optional<DomainId> rack = parse_domain_id("rack:3");
  MF_CHECK(rack.has_value());
  MF_CHECK(rack->kind == DomainKind::Rack);
  MF_CHECK(!parse_domain_id("rack").has_value());
  MF_CHECK(TargetId{TargetKind::Link, 1} < TargetId{TargetKind::Switch, 1});
}

MF_TEST(identity, incarnation_fencing) {
  ControllerIncarnation first;
  first.controller = ControllerId::from_u64(1);
  first.boot_epoch = BootEpoch::from_u64(4);
  first.nonce = 111;
  ControllerIncarnation second = first;
  second.boot_epoch = BootEpoch::from_u64(5);
  second.nonce = 222;
  MF_CHECK(is_fenced_by(first, second));
  MF_CHECK(!is_fenced_by(second, first));
  MF_CHECK(!is_fenced_by(second, second));
  MF_CHECK(is_fenced_by(first, first) == false);
  ControllerIncarnation other = second;
  other.nonce = 333;
  MF_CHECK(is_fenced_by(second, other));

  const std::string text = to_string(first);
  const std::optional<ControllerIncarnation> parsed = parse_controller_incarnation(text);
  MF_CHECK(parsed.has_value());
  MF_CHECK_EQ(*parsed, first);
}

MF_TEST(checked, arithmetic_never_wraps) {
  MF_CHECK(checked_add<std::uint32_t>(1, 2).value() == 3u);
  MF_CHECK(!checked_add<std::uint32_t>(0xFFFFFFFFu, 1u).has_value());
  MF_CHECK(!checked_mul<std::uint32_t>(0x10000u, 0x10000u).has_value());
  MF_CHECK(checked_mul<std::uint32_t>(0, 0xFFFFFFFFu).value() == 0u);
  MF_CHECK(!checked_sub<std::uint32_t>(1, 2).has_value());
  MF_CHECK(checked_mul_signed<std::int64_t>(1000000000LL, 1000000000LL).value() ==
           1000000000000000000LL);
  MF_CHECK(!checked_mul_signed<std::int64_t>(4000000000LL, 4000000000LL).has_value());
  MF_CHECK(!checked_mul_signed<std::int64_t>(-4000000000LL, 4000000000LL).has_value());
  MF_CHECK(checked_mul_signed<std::int64_t>(-3, 4).value() == -12);
  MF_CHECK(narrow<std::uint16_t>(65535).has_value());
  MF_CHECK(!narrow<std::uint16_t>(65536).has_value());
  MF_CHECK(!narrow<std::uint16_t>(-1).has_value());
}

MF_TEST(bytes, round_trip_and_bounds) {
  ByteWriter writer;
  writer.u8(0xAB);
  writer.u16(0x1234);
  writer.u32(0xDEADBEEF);
  writer.u64(0x0102030405060708ull);
  writer.i64(-42);
  writer.boolean(true);
  writer.str("hello", 16);
  const std::vector<std::byte> buffer = writer.take();

  ByteReader reader(buffer);
  MF_CHECK_EQ(reader.u8(), 0xABu);
  MF_CHECK_EQ(reader.u16(), 0x1234u);
  MF_CHECK_EQ(reader.u32(), 0xDEADBEEFu);
  MF_CHECK_EQ(reader.u64(), 0x0102030405060708ull);
  MF_CHECK_EQ(reader.i64(), -42);
  MF_CHECK_EQ(reader.boolean(), true);
  MF_CHECK_EQ(reader.str(16), std::string("hello"));
  MF_CHECK(reader.ok());
  MF_CHECK(reader.at_end());

  ByteReader short_reader(std::span<const std::byte>(buffer.data(), 3));
  (void)short_reader.u8();
  (void)short_reader.u16();
  (void)short_reader.u32();
  MF_CHECK(!short_reader.ok());

  ByteWriter bounded;
  bounded.str(std::string(64, 'x'), 8);
  MF_CHECK(bounded.overflowed());
  MF_CHECK_EQ(bounded.size(), 2u);

  ByteReader malicious(std::span<const std::byte>(buffer.data(), buffer.size()));
  (void)malicious.u8();
  (void)malicious.u16();
  (void)malicious.u32();
  (void)malicious.u64();
  (void)malicious.i64();
  (void)malicious.boolean();
  MF_CHECK_EQ(malicious.str(4), std::string());
  MF_CHECK(!malicious.ok());
}

MF_TEST(hash, deterministic_values) {
  MF_CHECK_EQ(crc32c("123456789"), 0xE3069283u);
  MF_CHECK_EQ(fnv1a64(""), 14695981039346656037ull);
  Digest first;
  first.update("abc");
  first.update_u32(7);
  Digest second;
  second.update("abc");
  second.update_u32(7);
  MF_CHECK_EQ(first.value(), second.value());
  Digest third;
  third.update_u32(7);
  third.update("abc");
  MF_CHECK_NE(first.value(), third.value());
  MF_CHECK_EQ(hex_u32(0xDEADBEEFu), std::string("deadbeef"));
  MF_CHECK_EQ(hex_u64(0), std::string("0000000000000000"));
}

MF_TEST(time, formatting_and_parsing) {
  const Nanos epoch = 1750000000LL * kNanosPerSecond;
  MF_CHECK_EQ(format_time(epoch), std::string("2025-06-15T15:06:40.000000000Z"));
  const std::optional<Nanos> parsed = parse_time("2025-06-15T15:06:40Z", 0);
  MF_CHECK(parsed.has_value());
  MF_CHECK_EQ(*parsed, epoch);
  MF_CHECK_EQ(*parse_time("epoch:1750000000", 0), epoch);
  MF_CHECK_EQ(*parse_time("+30s", epoch), epoch + 30 * kNanosPerSecond);
  MF_CHECK_EQ(*parse_time("-5m", epoch), epoch - 5 * kNanosPerMinute);
  MF_CHECK(!parse_time("2025-13-15T15:06:40Z", 0).has_value());
  MF_CHECK(!parse_time("2025-06-15T15:06:40+01:00", 0).has_value());
  MF_CHECK(!parse_time("garbage", 0).has_value());
  MF_CHECK_EQ(format_duration(90 * kNanosPerSecond), std::string("1m30s"));
  MF_CHECK_EQ(format_duration(500), std::string("500ns"));
}

MF_TEST(config, topology_round_trip) {
  const std::string text =
      "# comment\n"
      "domain kind=rack id=1 name=rack-a correlated=true\n"
      "target kind=switch id=1 name=sw-a domains=rack:1 capacity=1 maintainable=true\n"
      "target kind=link id=1 name=lag-1 parent=switch:1 domains=rack:1\n"
      "target kind=link id=2 name=lag-2 parent=switch:1 domains=rack:1\n"
      "pool id=1 name=up members=link:1,link:2\n";
  ParseStats stats;
  Result<Topology> topology = load_topology_text(text, stats);
  MF_CHECK_OK(topology);
  MF_CHECK_EQ(stats.records, 5u);
  MF_CHECK_EQ(topology.value().size(), 3u);
  MF_CHECK_EQ(topology.value().removal_set(std::vector<TargetId>{TargetId{TargetKind::Switch, 1}}).size(),
              3u);
  const Result<Topology> again = load_topology_text(render_topology(topology.value()), stats);
  MF_CHECK_OK(again);
  MF_CHECK_EQ(again.value().digest(), topology.value().digest());
}

MF_TEST(config, malformed_topology_is_rejected_with_a_line) {
  ParseStats stats;
  const auto expect_rejected = [&stats](const std::string& text) {
    const Result<Topology> result = load_topology_text(text, stats);
    MF_CHECK(!result.ok());
    MF_CHECK(result.error().detail.find("line ") != std::string::npos);
  };
  expect_rejected("domain kind=rack\n");
  expect_rejected("domain kind=rack id=1 name=x correlated=maybe\n");
  expect_rejected("domain kind=rack id=1 name=x name=y\n");
  expect_rejected("target kind=link id=1 domains=rack:9\n");
  expect_rejected("target kind=link id=1 parent=switch:1 domains=rack:1\n");
  expect_rejected("target kind=link id=1 domains=rack:1 extra=1\n");
  expect_rejected("widget kind=link id=1 domains=rack:1\n");
  expect_rejected("domain kind=rack id=1 nameless\n");
  expect_rejected("domain kind=rack id=1 name=x nosuchkey=1\n");
}

MF_TEST(config, malformed_policy_is_rejected) {
  ParseStats stats;
  const auto expect_rejected = [&stats](const std::string& text) {
    const Result<Policy> result = load_policy_text(text, 0, stats);
    MF_CHECK(!result.ok());
  };
  expect_rejected("redundancy pool=1 min_viable=0 tolerated_losses=1\n");
  expect_rejected("redundancy pool=1 min_viable=1\n");
  expect_rejected("domain_limit domain=rack:1 max_out=0\n");
  expect_rejected("window id=1 opens=2026-01-02T00:00:00Z closes=2026-01-01T00:00:00Z\n");
  expect_rejected("tier index=0 name=a\ntier index=0 name=b\n");
  expect_rejected("setting key=unknown_setting value=1\n");
  expect_rejected("evidence_ttl kind=target-health ttl=1ns\n");
}

MF_TEST(config, valid_policy_round_trip) {
  const std::string text =
      "tier index=0 name=emergency\n"
      "redundancy pool=1 min_viable=2 tolerated_losses=2 name=n-plus-2\n"
      "headroom pool=1 reserve=1 name=reserve\n"
      "domain_limit domain=rack:1 max_out=1\n"
      "contract id=1 name=web members=link:1,link:2 min_available=1\n"
      "window id=1 name=night opens=epoch:1000 closes=epoch:2000 min_lead=60s "
      "on_close=finish-active-step abort_grace=30s\n"
      "evidence_ttl kind=target-health ttl=15s\n"
      "setting key=min_evidence_sources value=2\n"
      "setting key=require_window value=true\n";
  ParseStats stats;
  Result<Policy> policy = load_policy_text(text, 0, stats);
  MF_CHECK_OK(policy);
  MF_CHECK_EQ(policy.value().redundancy.size(), 1u);
  MF_CHECK_EQ(policy.value().redundancy_for(PoolId::from_u64(1)).required_serving_units(), 4u);
  MF_CHECK_EQ(policy.value().min_evidence_sources, 2u);
  MF_CHECK(policy.value().require_window);
  MF_CHECK_EQ(policy.value().evidence_ttl.at(EvidenceKind::TargetHealth), 15 * kNanosPerSecond);
  const Result<Policy> again = load_policy_text(render_policy(policy.value()), 0, stats);
  MF_CHECK_OK(again);
  MF_CHECK_EQ(again.value().digest(), policy.value().digest());
}

MF_TEST(config, durations) {
  MF_CHECK_EQ(parse_duration_text("30s").value(), 30 * kNanosPerSecond);
  MF_CHECK_EQ(parse_duration_text("5m").value(), 5 * kNanosPerMinute);
  MF_CHECK_EQ(parse_duration_text("2h").value(), 2 * kNanosPerHour);
  MF_CHECK_EQ(parse_duration_text("250ms").value(), 250 * kNanosPerMillisecond);
  MF_CHECK_EQ(parse_duration_text("900").value(), 900);
  MF_CHECK(parse_duration_text("30").has_value());
  MF_CHECK(!parse_duration_text("s30").has_value());
  MF_CHECK(!parse_duration_text("12 parsecs").has_value());
  MF_CHECK(!parse_duration_text("99999999999999999999h").has_value());
}

MF_TEST_MAIN()
