// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/config/text.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "mf/core/checked.hpp"
#include "mf/core/id.hpp"
#include "mf/core/limits.hpp"

namespace mf {
namespace {

struct Token {
  std::string key;
  std::string value;
};

struct Line {
  std::size_t number{0};
  std::string kind;
  std::vector<Token> tokens;

  [[nodiscard]] const std::string* find(std::string_view name) const {
    for (const Token& token : tokens) {
      if (token.key == name) {
        return &token.value;
      }
    }
    return nullptr;
  }
};

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
    ++begin;
  }
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string with_line(std::size_t line, std::string_view message) {
  return "line " + std::to_string(line) + ": " + std::string(message);
}

Result<std::vector<Line>> tokenize(std::string_view text) {
  std::vector<Line> lines;
  std::size_t number = 0;
  std::size_t position = 0;
  while (position <= text.size()) {
    const std::size_t newline = text.find('\n', position);
    const std::string_view raw =
        newline == std::string_view::npos ? text.substr(position) : text.substr(position, newline - position);
    ++number;
    std::string body = trim(raw);
    const std::size_t comment = body.find('#');
    if (comment != std::string::npos) {
      body = trim(body.substr(0, comment));
    }
    if (!body.empty()) {
      Line line;
      line.number = number;
      std::istringstream stream(body);
      std::string token;
      bool first = true;
      while (stream >> token) {
        if (first) {
          line.kind = token;
          first = false;
          continue;
        }
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos || equals == 0 || equals + 1 >= token.size()) {
          return make_error(ErrorCode::InvalidArgument,
                            with_line(number, "token '" + token + "' is not key=value"));
        }
        Token entry;
        entry.key = token.substr(0, equals);
        entry.value = token.substr(equals + 1);
        if (line.find(entry.key) != nullptr) {
          return make_error(ErrorCode::InvalidArgument,
                            with_line(number, "key '" + entry.key + "' is repeated"));
        }
        line.tokens.push_back(std::move(entry));
      }
      if (line.tokens.size() > 64) {
        return make_error(ErrorCode::LimitExceeded, with_line(number, "too many keys on one line"));
      }
      lines.push_back(std::move(line));
    }
    if (newline == std::string_view::npos) {
      break;
    }
    position = newline + 1;
  }
  return lines;
}

Status require_known_keys(const Line& line, const std::vector<std::string_view>& allowed) {
  for (const Token& token : line.tokens) {
    const bool known = std::any_of(allowed.begin(), allowed.end(),
                                   [&token](std::string_view name) { return token.key == name; });
    if (!known) {
      return make_error(ErrorCode::InvalidArgument,
                        with_line(line.number, "unknown key '" + token.key + "' for record '" +
                                                   line.kind + "'"));
    }
  }
  return ok_status();
}

Result<std::string> require_value(const Line& line, std::string_view key) {
  const std::string* value = line.find(key);
  if (value == nullptr) {
    return make_error(ErrorCode::InvalidArgument,
                      with_line(line.number, "missing required key '" + std::string(key) + "'"));
  }
  return *value;
}

Result<bool> parse_bool(const Line& line, const Token& token) {
  if (token.value == "true" || token.value == "1" || token.value == "yes") {
    return true;
  }
  if (token.value == "false" || token.value == "0" || token.value == "no") {
    return false;
  }
  return make_error(ErrorCode::InvalidArgument,
                    with_line(line.number, "value '" + token.value + "' is not a boolean"));
}

Result<std::vector<TargetId>> parse_target_list(const Line& line, const Token& token) {
  std::vector<TargetId> out;
  std::size_t position = 0;
  const std::string& text = token.value;
  while (position <= text.size()) {
    const std::size_t comma = text.find(',', position);
    const std::string piece = comma == std::string::npos ? text.substr(position)
                                                         : text.substr(position, comma - position);
    if (piece.empty()) {
      return make_error(ErrorCode::InvalidArgument,
                        with_line(line.number, "target list contains an empty entry"));
    }
    const std::optional<TargetId> target = parse_target_id(piece);
    if (!target.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        with_line(line.number, "target '" + piece + "' is not a target id"));
    }
    out.push_back(*target);
    if (comma == std::string::npos) {
      break;
    }
    position = comma + 1;
  }
  if (out.empty()) {
    return make_error(ErrorCode::InvalidArgument, with_line(line.number, "target list is empty"));
  }
  return out;
}

Result<std::vector<DomainId>> parse_domain_list(const Line& line, const Token& token) {
  std::vector<DomainId> out;
  std::size_t position = 0;
  const std::string& text = token.value;
  while (position <= text.size()) {
    const std::size_t comma = text.find(',', position);
    const std::string piece = comma == std::string::npos ? text.substr(position)
                                                         : text.substr(position, comma - position);
    if (piece.empty()) {
      return make_error(ErrorCode::InvalidArgument,
                        with_line(line.number, "domain list contains an empty entry"));
    }
    const std::optional<DomainId> domain = parse_domain_id(piece);
    if (!domain.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        with_line(line.number, "domain '" + piece + "' is not a domain id"));
    }
    out.push_back(*domain);
    if (comma == std::string::npos) {
      break;
    }
    position = comma + 1;
  }
  if (out.empty()) {
    return make_error(ErrorCode::InvalidArgument, with_line(line.number, "domain list is empty"));
  }
  return out;
}

Result<std::uint32_t> parse_u32_value(const Line& line, const Token& token) {
  const std::optional<std::uint32_t> value = parse_u32(token.value);
  if (!value.has_value()) {
    return make_error(ErrorCode::InvalidArgument,
                      with_line(line.number, "value '" + token.value + "' is not a number"));
  }
  return *value;
}

}  // namespace

std::optional<Nanos> parse_duration_text(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t digits = 0;
  while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
    ++digits;
  }
  if (digits == 0) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> magnitude = parse_u64(text.substr(0, digits));
  if (!magnitude.has_value()) {
    return std::nullopt;
  }
  const std::string_view unit = text.substr(digits);
  Nanos scale = 1;
  if (unit == "s") {
    scale = kNanosPerSecond;
  } else if (unit == "m") {
    scale = kNanosPerMinute;
  } else if (unit == "h") {
    scale = kNanosPerHour;
  } else if (unit == "ms") {
    scale = kNanosPerMillisecond;
  } else if (unit == "us") {
    scale = kNanosPerMicrosecond;
  } else if (unit == "ns" || unit.empty()) {
    scale = 1;
  } else {
    return std::nullopt;
  }
  const std::optional<Nanos> narrowed = narrow<Nanos>(*magnitude);
  if (!narrowed.has_value()) {
    return std::nullopt;
  }
  const std::optional<Nanos> product = checked_mul_signed<Nanos>(*narrowed, scale);
  if (!product.has_value()) {
    return std::nullopt;
  }
  return product;
}

Result<Topology> load_topology_text(std::string_view text, ParseStats& stats) {
  Result<std::vector<Line>> tokenized = tokenize(text);
  if (!tokenized.ok()) {
    return tokenized.error();
  }
  Topology topology;
  stats = ParseStats{};
  stats.lines = tokenized.value().size();

  for (const Line& line : tokenized.value()) {
    if (line.kind == "domain") {
      const Status keys = require_known_keys(line, {"kind", "id", "name", "correlated"});
      if (!keys.ok()) {
        return keys.error();
      }
      Result<std::string> kind_text = require_value(line, "kind");
      if (!kind_text.ok()) {
        return kind_text.error();
      }
      Result<std::string> id_text = require_value(line, "id");
      if (!id_text.ok()) {
        return id_text.error();
      }
      const std::optional<DomainKind> kind = parse_domain_kind(kind_text.value());
      const std::optional<std::uint32_t> index = parse_u32(id_text.value());
      if (!kind.has_value() || *kind == DomainKind::Unknown || !index.has_value()) {
        return make_error(ErrorCode::InvalidArgument,
                          with_line(line.number, "domain kind/id is not valid"));
      }
      DomainRecord record;
      record.id = DomainId{*kind, *index};
      if (const std::string* name = line.find("name"); name != nullptr) {
        record.name = *name;
      } else {
        record.name = to_string(record.id);
      }
      if (const std::string* correlated = line.find("correlated"); correlated != nullptr) {
        const Token token{"correlated", *correlated};
        Result<bool> parsed = parse_bool(line, token);
        if (!parsed.ok()) {
          return parsed.error();
        }
        record.correlated = parsed.value();
      }
      const Status added = topology.add_domain(std::move(record));
      if (!added.ok()) {
        return make_error(added.error().code, with_line(line.number, added.error().detail));
      }
      ++stats.records;
      continue;
    }
    if (line.kind == "target") {
      const Status keys = require_known_keys(
          line, {"kind", "id", "name", "parent", "domains", "capacity", "maintainable"});
      if (!keys.ok()) {
        return keys.error();
      }
      Result<std::string> kind_text = require_value(line, "kind");
      if (!kind_text.ok()) {
        return kind_text.error();
      }
      Result<std::string> id_text = require_value(line, "id");
      if (!id_text.ok()) {
        return id_text.error();
      }
      Result<std::string> domains_text = require_value(line, "domains");
      if (!domains_text.ok()) {
        return domains_text.error();
      }
      const std::optional<TargetKind> kind = parse_target_kind(kind_text.value());
      const std::optional<std::uint32_t> index = parse_u32(id_text.value());
      if (!kind.has_value() || *kind == TargetKind::Unknown || !index.has_value()) {
        return make_error(ErrorCode::InvalidArgument,
                          with_line(line.number, "target kind/id is not valid"));
      }
      TargetRecord record;
      record.id = TargetId{*kind, *index};
      record.name = line.find("name") != nullptr ? *line.find("name") : to_string(record.id);
      Result<std::vector<DomainId>> domains =
          parse_domain_list(line, Token{"domains", domains_text.value()});
      if (!domains.ok()) {
        return domains.error();
      }
      record.domains = domains.value();
      if (const std::string* parent = line.find("parent"); parent != nullptr) {
        const std::optional<TargetId> parsed = parse_target_id(*parent);
        if (!parsed.has_value()) {
          return make_error(ErrorCode::InvalidArgument,
                            with_line(line.number, "parent is not a target id"));
        }
        record.parent = *parsed;
      }
      if (const std::string* capacity = line.find("capacity"); capacity != nullptr) {
        Result<std::uint32_t> parsed = parse_u32_value(line, Token{"capacity", *capacity});
        if (!parsed.ok()) {
          return parsed.error();
        }
        record.capacity_units = parsed.value();
      }
      if (const std::string* maintainable = line.find("maintainable"); maintainable != nullptr) {
        Result<bool> parsed = parse_bool(line, Token{"maintainable", *maintainable});
        if (!parsed.ok()) {
          return parsed.error();
        }
        record.maintainable = parsed.value();
      }
      const Status added = topology.add_target(std::move(record));
      if (!added.ok()) {
        return make_error(added.error().code, with_line(line.number, added.error().detail));
      }
      ++stats.records;
      continue;
    }
    if (line.kind == "pool") {
      const Status keys = require_known_keys(line, {"id", "name", "members"});
      if (!keys.ok()) {
        return keys.error();
      }
      Result<std::string> id_text = require_value(line, "id");
      if (!id_text.ok()) {
        return id_text.error();
      }
      Result<std::string> members_text = require_value(line, "members");
      if (!members_text.ok()) {
        return members_text.error();
      }
      const std::optional<std::uint32_t> index = parse_u32(id_text.value());
      if (!index.has_value()) {
        return make_error(ErrorCode::InvalidArgument, with_line(line.number, "pool id is not valid"));
      }
      PoolRecord record;
      record.id = PoolId::from_u64(*index);
      record.name = line.find("name") != nullptr ? *line.find("name") : to_string(record.id);
      Result<std::vector<TargetId>> members =
          parse_target_list(line, Token{"members", members_text.value()});
      if (!members.ok()) {
        return members.error();
      }
      record.members = members.value();
      const Status added = topology.add_pool(std::move(record));
      if (!added.ok()) {
        return make_error(added.error().code, with_line(line.number, added.error().detail));
      }
      ++stats.records;
      continue;
    }
    return make_error(ErrorCode::InvalidArgument,
                      with_line(line.number, "unknown record kind '" + line.kind + "'"));
  }
  const Status valid = topology.validate();
  if (!valid.ok()) {
    return make_error(valid.error().code, valid.error().detail);
  }
  return topology;
}

Result<Topology> load_topology_file(const std::string& path, ParseStats& stats) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::NotFound, "cannot open topology file: " + path);
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const std::string text = buffer.str();
  if (text.size() > limits::kMaxSnapshotBytes) {
    return make_error(ErrorCode::LimitExceeded, "topology file exceeds the accepted size");
  }
  return load_topology_text(text, stats);
}

Result<Policy> load_policy_text(std::string_view text, Nanos reference_now, ParseStats& stats) {
  Result<std::vector<Line>> tokenized = tokenize(text);
  if (!tokenized.ok()) {
    return tokenized.error();
  }
  Policy policy;
  stats = ParseStats{};
  stats.lines = tokenized.value().size();
  std::map<std::string, std::string> settings;

  for (const Line& line : tokenized.value()) {
    if (line.kind == "redundancy") {
      const Status keys = require_known_keys(line, {"pool", "min_viable", "tolerated_losses", "name"});
      if (!keys.ok()) {
        return keys.error();
      }
      Result<std::string> pool_text = require_value(line, "pool");
      if (!pool_text.ok()) {
        return pool_text.error();
      }
      Result<std::string> viable_text = require_value(line, "min_viable");
      if (!viable_text.ok()) {
        return viable_text.error();
      }
      Result<std::string> losses_text = require_value(line, "tolerated_losses");
      if (!losses_text.ok()) {
        return losses_text.error();
      }
      const std::optional<std::uint32_t> pool = parse_u32(pool_text.value());
      Result<std::uint32_t> viable = parse_u32_value(line, Token{"min_viable", viable_text.value()});
      if (!viable.ok()) {
        return viable.error();
      }
      Result<std::uint32_t> losses =
          parse_u32_value(line, Token{"tolerated_losses", losses_text.value()});
      if (!losses.ok()) {
        return losses.error();
      }
      if (!pool.has_value()) {
        return make_error(ErrorCode::InvalidArgument, with_line(line.number, "pool id is not valid"));
      }
      RedundancyRule rule;
      rule.pool = PoolId::from_u64(*pool);
      rule.min_viable = viable.value();
      rule.tolerated_losses = losses.value();
      rule.name = line.find("name") != nullptr ? *line.find("name") : "redundancy";
      policy.redundancy.push_back(std::move(rule));
      ++stats.records;
      continue;
    }
    if (line.kind == "headroom") {
      const Status keys = require_known_keys(line, {"pool", "reserve", "name"});
      if (!keys.ok()) {
        return keys.error();
      }
      Result<std::string> pool_text = require_value(line, "pool");
      if (!pool_text.ok()) {
        return pool_text.error();
      }
      Result<std::string> reserve_text = require_value(line, "reserve");
      if (!reserve_text.ok()) {
        return reserve_text.error();
      }
      const std::optional<std::uint32_t> pool = parse_u32(pool_text.value());
      Result<std::uint32_t> reserve = parse_u32_value(line, Token{"reserve", reserve_text.value()});
      if (!reserve.ok()) {
        return reserve.error();
      }
      if (!pool.has_value()) {
        return make_error(ErrorCode::InvalidArgument, with_line(line.number, "pool id is not valid"));
      }
      HeadroomRule rule;
      rule.pool = PoolId::from_u64(*pool);
      rule.reserve_units = reserve.value();
      rule.name = line.find("name") != nullptr ? *line.find("name") : "reserve";
      policy.headroom.push_back(std::move(rule));
      ++stats.records;
      continue;
    }
    if (line.kind == "domain_limit") {
      const Status keys = require_known_keys(line, {"domain", "max_out", "name"});
      if (!keys.ok()) {
        return keys.error();
      }
      Result<std::string> domain_text = require_value(line, "domain");
      if (!domain_text.ok()) {
        return domain_text.error();
      }
      Result<std::string> max_text = require_value(line, "max_out");
      if (!max_text.ok()) {
        return max_text.error();
      }
      const std::optional<DomainId> domain = parse_domain_id(domain_text.value());
      Result<std::uint32_t> max_out = parse_u32_value(line, Token{"max_out", max_text.value()});
      if (!max_out.ok()) {
        return max_out.error();
      }
      if (!domain.has_value()) {
        return make_error(ErrorCode::InvalidArgument,
                          with_line(line.number, "domain id is not valid"));
      }
      DomainLimitRule rule;
      rule.domain = *domain;
      rule.max_out = max_out.value();
      rule.name = line.find("name") != nullptr ? *line.find("name") : "domain-limit";
      policy.domain_limits.push_back(std::move(rule));
      ++stats.records;
      continue;
    }
    if (line.kind == "contract") {
      const Status keys =
          require_known_keys(line, {"id", "name", "members", "min_available", "fresh_evidence"});
      if (!keys.ok()) {
        return keys.error();
      }
      Result<std::string> id_text = require_value(line, "id");
      if (!id_text.ok()) {
        return id_text.error();
      }
      Result<std::string> members_text = require_value(line, "members");
      if (!members_text.ok()) {
        return members_text.error();
      }
      Result<std::string> min_text = require_value(line, "min_available");
      if (!min_text.ok()) {
        return min_text.error();
      }
      const std::optional<std::uint32_t> id = parse_u32(id_text.value());
      Result<std::vector<TargetId>> members =
          parse_target_list(line, Token{"members", members_text.value()});
      if (!members.ok()) {
        return members.error();
      }
      Result<std::uint32_t> min_available =
          parse_u32_value(line, Token{"min_available", min_text.value()});
      if (!min_available.ok()) {
        return min_available.error();
      }
      if (!id.has_value()) {
        return make_error(ErrorCode::InvalidArgument,
                          with_line(line.number, "contract id is not valid"));
      }
      Contract contract;
      contract.id = ContractId::from_u64(*id);
      contract.name = line.find("name") != nullptr ? *line.find("name") : "contract";
      contract.members = members.value();
      contract.min_available = min_available.value();
      if (const std::string* fresh = line.find("fresh_evidence"); fresh != nullptr) {
        Result<bool> parsed = parse_bool(line, Token{"fresh_evidence", *fresh});
        if (!parsed.ok()) {
          return parsed.error();
        }
        contract.require_fresh_evidence = parsed.value();
      }
      policy.contracts.push_back(std::move(contract));
      ++stats.records;
      continue;
    }
    if (line.kind == "window") {
      const Status keys = require_known_keys(
          line, {"id", "name", "opens", "closes", "min_lead", "abort_grace", "on_close", "targets"});
      if (!keys.ok()) {
        return keys.error();
      }
      Result<std::string> id_text = require_value(line, "id");
      if (!id_text.ok()) {
        return id_text.error();
      }
      Result<std::string> opens_text = require_value(line, "opens");
      if (!opens_text.ok()) {
        return opens_text.error();
      }
      Result<std::string> closes_text = require_value(line, "closes");
      if (!closes_text.ok()) {
        return closes_text.error();
      }
      const std::optional<std::uint32_t> id = parse_u32(id_text.value());
      const std::optional<Nanos> opens = parse_time(opens_text.value(), reference_now);
      const std::optional<Nanos> closes = parse_time(closes_text.value(), reference_now);
      if (!id.has_value() || !opens.has_value() || !closes.has_value()) {
        return make_error(ErrorCode::InvalidArgument,
                          with_line(line.number, "window id or time is not valid"));
      }
      Window window;
      window.id = WindowId::from_u64(*id);
      window.name = line.find("name") != nullptr ? *line.find("name") : "window";
      window.opens_at = *opens;
      window.closes_at = *closes;
      if (const std::string* lead = line.find("min_lead"); lead != nullptr) {
        const std::optional<Nanos> parsed = parse_duration_text(*lead);
        if (!parsed.has_value()) {
          return make_error(ErrorCode::InvalidArgument,
                            with_line(line.number, "min_lead is not a duration"));
        }
        window.min_lead_time = *parsed;
      }
      if (const std::string* grace = line.find("abort_grace"); grace != nullptr) {
        const std::optional<Nanos> parsed = parse_duration_text(*grace);
        if (!parsed.has_value()) {
          return make_error(ErrorCode::InvalidArgument,
                            with_line(line.number, "abort_grace is not a duration"));
        }
        window.abort_grace = *parsed;
      }
      if (const std::string* on_close = line.find("on_close"); on_close != nullptr) {
        const std::optional<OnWindowClose> parsed = parse_on_window_close(*on_close);
        if (!parsed.has_value()) {
          return make_error(ErrorCode::InvalidArgument,
                            with_line(line.number, "on_close is not a known policy"));
        }
        window.on_close = *parsed;
      }
      if (const std::string* targets = line.find("targets"); targets != nullptr) {
        Result<std::vector<TargetId>> parsed =
            parse_target_list(line, Token{"targets", *targets});
        if (!parsed.ok()) {
          return parsed.error();
        }
        window.targets = parsed.value();
      }
      policy.windows.push_back(std::move(window));
      ++stats.records;
      continue;
    }
    if (line.kind == "tier") {
      const Status keys = require_known_keys(line, {"index", "name"});
      if (!keys.ok()) {
        return keys.error();
      }
      Result<std::string> index_text = require_value(line, "index");
      if (!index_text.ok()) {
        return index_text.error();
      }
      Result<std::uint32_t> index = parse_u32_value(line, Token{"index", index_text.value()});
      if (!index.ok()) {
        return index.error();
      }
      PriorityTier tier;
      tier.index = index.value();
      tier.name = line.find("name") != nullptr ? *line.find("name") : "tier";
      policy.tiers.push_back(std::move(tier));
      ++stats.records;
      continue;
    }
    if (line.kind == "evidence_ttl") {
      const Status keys = require_known_keys(line, {"kind", "ttl"});
      if (!keys.ok()) {
        return keys.error();
      }
      Result<std::string> kind_text = require_value(line, "kind");
      if (!kind_text.ok()) {
        return kind_text.error();
      }
      Result<std::string> ttl_text = require_value(line, "ttl");
      if (!ttl_text.ok()) {
        return ttl_text.error();
      }
      const std::optional<EvidenceKind> kind = parse_evidence_kind(kind_text.value());
      const std::optional<Nanos> ttl = parse_duration_text(ttl_text.value());
      if (!kind.has_value() || !ttl.has_value()) {
        return make_error(ErrorCode::InvalidArgument,
                          with_line(line.number, "evidence_ttl kind or ttl is not valid"));
      }
      policy.evidence_ttl[*kind] = *ttl;
      ++stats.records;
      continue;
    }
    if (line.kind == "setting") {
      const Status keys = require_known_keys(line, {"key", "value"});
      if (!keys.ok()) {
        return keys.error();
      }
      Result<std::string> key_text = require_value(line, "key");
      if (!key_text.ok()) {
        return key_text.error();
      }
      Result<std::string> value_text = require_value(line, "value");
      if (!value_text.ok()) {
        return value_text.error();
      }
      settings[key_text.value()] = value_text.value();
      ++stats.records;
      continue;
    }
    return make_error(ErrorCode::InvalidArgument,
                      with_line(line.number, "unknown record kind '" + line.kind + "'"));
  }

  for (const auto& [key, value] : settings) {
    if (key == "min_evidence_sources") {
      const std::optional<std::uint32_t> parsed = parse_u32(value);
      if (!parsed.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "min_evidence_sources is not a number");
      }
      policy.min_evidence_sources = *parsed;
    } else if (key == "require_window") {
      if (value == "true" || value == "1") {
        policy.require_window = true;
      } else if (value == "false" || value == "0") {
        policy.require_window = false;
      } else {
        return make_error(ErrorCode::InvalidArgument, "require_window is not a boolean");
      }
    } else if (key == "max_precondition_age") {
      const std::optional<Nanos> parsed = parse_duration_text(value);
      if (!parsed.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "max_precondition_age is not a duration");
      }
      policy.max_precondition_age = *parsed;
    } else if (key == "default_duration") {
      const std::optional<Nanos> parsed = parse_duration_text(value);
      if (!parsed.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "default_duration is not a duration");
      }
      policy.default_duration = *parsed;
    } else if (key == "max_concurrent_jobs") {
      const std::optional<std::uint32_t> parsed = parse_u32(value);
      if (!parsed.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "max_concurrent_jobs is not a number");
      }
      policy.max_concurrent_jobs = *parsed;
    } else if (key == "authority_lease") {
      const std::optional<Nanos> parsed = parse_duration_text(value);
      if (!parsed.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "authority_lease is not a duration");
      }
      policy.authority_lease = *parsed;
    } else if (key == "drain_lease") {
      const std::optional<Nanos> parsed = parse_duration_text(value);
      if (!parsed.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "drain_lease is not a duration");
      }
      policy.drain_lease = *parsed;
    } else if (key == "max_drain_failures") {
      const std::optional<std::uint32_t> parsed = parse_u32(value);
      if (!parsed.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "max_drain_failures is not a number");
      }
      policy.max_drain_failures_before_block = *parsed;
    } else if (key == "max_verification_attempts") {
      const std::optional<std::uint32_t> parsed = parse_u32(value);
      if (!parsed.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "max_verification_attempts is not a number");
      }
      policy.max_verification_attempts = *parsed;
    } else if (key == "max_restoration_attempts") {
      const std::optional<std::uint32_t> parsed = parse_u32(value);
      if (!parsed.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "max_restoration_attempts is not a number");
      }
      policy.max_restoration_attempts = *parsed;
    } else {
      return make_error(ErrorCode::InvalidArgument, "unknown setting '" + key + "'");
    }
  }

  const Status valid = policy.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  return policy;
}

Result<Policy> load_policy_file(const std::string& path, Nanos reference_now, ParseStats& stats) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::NotFound, "cannot open policy file: " + path);
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const std::string text = buffer.str();
  if (text.size() > limits::kMaxSnapshotBytes) {
    return make_error(ErrorCode::LimitExceeded, "policy file exceeds the accepted size");
  }
  return load_policy_text(text, reference_now, stats);
}

std::string render_topology(const Topology& topology) {
  std::string out;
  for (const auto& [id, record] : topology.domains()) {
    out += "domain kind=" + std::string(to_string(id.kind)) + " id=" + std::to_string(id.index) +
           " name=" + record.name + " correlated=" + (record.correlated ? "true" : "false") + "\n";
  }
  // Parents must be declared before their children, so targets are emitted in
  // containment order (depth first, then identity).
  std::vector<std::pair<std::size_t, TargetId>> ordered;
  ordered.reserve(topology.targets().size());
  for (const auto& [id, record] : topology.targets()) {
    (void)record;
    ordered.emplace_back(topology.ancestors(id).size(), id);
  }
  std::sort(ordered.begin(), ordered.end());
  for (const auto& [depth, id] : ordered) {
    (void)depth;
    const TargetRecord& record = topology.targets().at(id);
    out += "target kind=" + std::string(to_string(id.kind)) + " id=" + std::to_string(id.index) +
           " name=" + record.name;
    if (record.parent.has_value()) {
      out += " parent=" + to_string(*record.parent);
    }
    out += " domains=";
    for (std::size_t i = 0; i < record.domains.size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      out += to_string(record.domains[i]);
    }
    out += " capacity=" + std::to_string(record.capacity_units);
    out += std::string(" maintainable=") + (record.maintainable ? "true" : "false") + "\n";
  }
  for (const auto& [id, record] : topology.pools()) {
    out += "pool id=" + std::to_string(id.value()) + " name=" + record.name + " members=";
    for (std::size_t i = 0; i < record.members.size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      out += to_string(record.members[i]);
    }
    out.push_back('\n');
  }
  return out;
}

std::string render_policy(const Policy& policy) {
  std::string out;
  for (const auto& [kind, ttl] : policy.evidence_ttl) {
    out += "evidence_ttl kind=" + std::string(to_string(kind)) + " ttl=" + std::to_string(ttl) +
           "ns\n";
  }
  for (const RedundancyRule& rule : policy.redundancy) {
    out += "redundancy pool=" + std::to_string(rule.pool.value()) +
           " min_viable=" + std::to_string(rule.min_viable) +
           " tolerated_losses=" + std::to_string(rule.tolerated_losses) + " name=" + rule.name +
           "\n";
  }
  for (const HeadroomRule& rule : policy.headroom) {
    out += "headroom pool=" + std::to_string(rule.pool.value()) +
           " reserve=" + std::to_string(rule.reserve_units) + " name=" + rule.name + "\n";
  }
  for (const DomainLimitRule& rule : policy.domain_limits) {
    out += "domain_limit domain=" + to_string(rule.domain) +
           " max_out=" + std::to_string(rule.max_out) + " name=" + rule.name + "\n";
  }
  for (const Contract& contract : policy.contracts) {
    out += "contract id=" + std::to_string(contract.id.value()) + " name=" + contract.name +
           " members=";
    for (std::size_t i = 0; i < contract.members.size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      out += to_string(contract.members[i]);
    }
    out += " min_available=" + std::to_string(contract.min_available) + "\n";
  }
  for (const Window& window : policy.windows) {
    // Durations round-trip as explicit nanosecond counts so the rendered policy
    // is always re-loadable.
    out += "window id=" + std::to_string(window.id.value()) + " name=" + window.name +
           " opens=" + format_time(window.opens_at) + " closes=" + format_time(window.closes_at) +
           " min_lead=" + std::to_string(window.min_lead_time) + "ns" +
           " abort_grace=" + std::to_string(window.abort_grace) + "ns" +
           " on_close=" + to_string(window.on_close);
    if (!window.targets.empty()) {
      out += " targets=";
      for (std::size_t i = 0; i < window.targets.size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        out += to_string(window.targets[i]);
      }
    }
    out.push_back('\n');
  }
  for (const PriorityTier& tier : policy.tiers) {
    out += "tier index=" + std::to_string(tier.index) + " name=" + tier.name + "\n";
  }
  out += "setting key=min_evidence_sources value=" + std::to_string(policy.min_evidence_sources) +
         "\n";
  out += std::string("setting key=require_window value=") +
         (policy.require_window ? "true" : "false") + "\n";
  out += "setting key=max_precondition_age value=" +
         std::to_string(policy.max_precondition_age) + "ns\n";
  out += "setting key=default_duration value=" + std::to_string(policy.default_duration) + "ns\n";
  out += "setting key=max_concurrent_jobs value=" + std::to_string(policy.max_concurrent_jobs) +
         "\n";
  out += "setting key=authority_lease value=" + std::to_string(policy.authority_lease) + "ns\n";
  out += "setting key=drain_lease value=" + std::to_string(policy.drain_lease) + "ns\n";
  out += "setting key=max_drain_failures value=" +
         std::to_string(policy.max_drain_failures_before_block) + "\n";
  out += "setting key=max_verification_attempts value=" +
         std::to_string(policy.max_verification_attempts) + "\n";
  out += "setting key=max_restoration_attempts value=" +
         std::to_string(policy.max_restoration_attempts) + "\n";
  return out;
}

}  // namespace mf
