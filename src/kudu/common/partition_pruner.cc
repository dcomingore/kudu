// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/common/partition_pruner.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include <glog/logging.h>

#include "kudu/common/column_predicate.h"
#include "kudu/common/common.pb.h"
#include "kudu/common/encoded_key.h"
#include "kudu/common/key_encoder.h"
#include "kudu/common/key_util.h"
#include "kudu/common/partition.h"
#include "kudu/common/row.h"
#include "kudu/common/scan_spec.h"
#include "kudu/common/schema.h"
#include "kudu/common/types.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/array_view.h"
#include "kudu/util/memory/arena.h"
#include "kudu/util/slice.h"

using std::distance;
using std::find;
using std::iota;
using std::lower_bound;
using std::memcpy;
using std::move;
using std::string;
using std::unique_ptr;
using std::unordered_map;
using std::vector;

namespace kudu {
namespace {

// Returns true if the partition schema's range columns are a prefix of the
// primary key columns.
bool AreRangeColumnsPrefixOfPrimaryKey(const Schema& schema,
                                       const vector<ColumnId>& range_columns) {
  CHECK(range_columns.size() <= schema.num_key_columns());
  for (int32_t col_idx = 0; col_idx < range_columns.size(); ++col_idx) {
    if (schema.column_id(col_idx) != range_columns[col_idx]) {
      return false;
    }
  }
  return true;
}

// Translates the scan primary key bounds into range keys. This should only be
// used when the range columns are a prefix of the primary key columns.
void EncodeRangeKeysFromPrimaryKeyBounds(const Schema& schema,
                                         const ScanSpec& scan_spec,
                                         size_t num_range_columns,
                                         string* range_key_start,
                                         string* range_key_end) {
  if (scan_spec.lower_bound_key() == nullptr && scan_spec.exclusive_upper_bound_key() == nullptr) {
    // Don't bother if there are no lower and upper PK bounds
    return;
  }

  if (num_range_columns == schema.num_key_columns()) {
    // The range columns are the primary key columns, so the range key is the
    // primary key.
    if (scan_spec.lower_bound_key() != nullptr) {
      *range_key_start = scan_spec.lower_bound_key()->encoded_key().ToString();
    }
    if (scan_spec.exclusive_upper_bound_key() != nullptr) {
      *range_key_end = scan_spec.exclusive_upper_bound_key()->encoded_key().ToString();
    }
  } else {
    // The range-partition key columns are a prefix of the primary key columns.
    // Copy the column values over to a row, and then encode the row as a range
    // key.

    vector<int32_t> col_idxs(num_range_columns);
    iota(col_idxs.begin(), col_idxs.end(), 0);

    unique_ptr<uint8_t[]> buf(new uint8_t[schema.key_byte_size()]);
    ContiguousRow row(&schema, buf.get());

    if (scan_spec.lower_bound_key() != nullptr) {
      for (int32_t idx : col_idxs) {
        memcpy(row.mutable_cell_ptr(idx),
               scan_spec.lower_bound_key()->raw_keys()[idx],
               schema.column(idx).type_info()->size());
      }
      key_util::EncodeKey(col_idxs, row, range_key_start);
    }

    if (scan_spec.exclusive_upper_bound_key() != nullptr) {
      for (int32_t idx : col_idxs) {
        memcpy(row.mutable_cell_ptr(idx),
               scan_spec.exclusive_upper_bound_key()->raw_keys()[idx],
               schema.column(idx).type_info()->size());
      }

      // Determine if the upper bound primary key columns which aren't in the
      // range-partition key are all set to the minimum value. If so, the
      // range-partition key prefix of the primary key is already effectively an
      // exclusive bound. If not, then we increment the range-key prefix in
      // order to transform it from inclusive to exclusive.
      bool min_suffix = true;
      for (int32_t idx = num_range_columns; idx < schema.num_key_columns(); ++idx) {
        min_suffix &= schema.column(idx)
                            .type_info()
                            ->IsMinValue(scan_spec.exclusive_upper_bound_key()->raw_keys()[idx]);
      }
      Arena arena(std::max<size_t>(Arena::kMinimumChunkSize, schema.key_byte_size()));
      if (!min_suffix) {
        if (!key_util::IncrementPrimaryKey(&row, num_range_columns, &arena)) {
          // The range-partition key upper bound can't be incremented, which
          // means it's an inclusive bound on the maximum possible value, so
          // skip it.
          return;
        }
      }

      key_util::EncodeKey(col_idxs, row, range_key_end);
    }
  }
}

// Push the scan predicates into the range keys.
void EncodeRangeKeysFromPredicates(const Schema& schema,
                                   const unordered_map<string, ColumnPredicate>& predicates,
                                   const vector<ColumnId>& range_columns,
                                   string* range_key_start,
                                   string* range_key_end) {
  // Find the column indexes of the range columns.
  vector<int32_t> col_idxs;
  col_idxs.reserve(range_columns.size());
  for (const auto& column : range_columns) {
    int32_t col_idx = schema.find_column_by_id(column);
    CHECK_NE(Schema::kColumnNotFound, col_idx);
    CHECK_LT(col_idx, schema.num_key_columns());
    col_idxs.push_back(col_idx);
  }

  // Arenas must be at least the minimum chunk size, and we require at least
  // enough space for the range key columns.
  Arena arena(std::max<size_t>(Arena::kMinimumChunkSize, schema.key_byte_size()));
  uint8_t* buf = static_cast<uint8_t*>(CHECK_NOTNULL(arena.AllocateBytes(schema.key_byte_size())));
  ContiguousRow row(&schema, buf);

  if (key_util::PushLowerBoundKeyPredicates(col_idxs, predicates, &row, &arena) > 0) {
    key_util::EncodeKey(col_idxs, row, range_key_start);
  }

  if (key_util::PushUpperBoundKeyPredicates(col_idxs, predicates, &row, &arena) > 0) {
    key_util::EncodeKey(col_idxs, row, range_key_end);
  }
}
} // anonymous namespace

vector<bool> PartitionPruner::PruneHashComponent(
    const PartitionSchema::HashDimension& hash_dimension,
    const Schema& schema,
    const ScanSpec& scan_spec) {
  vector<bool> hash_bucket_bitset(hash_dimension.num_buckets, false);
  vector<string> encoded_strings(1, "");
  for (size_t col_offset = 0; col_offset < hash_dimension.column_ids.size(); ++col_offset) {
    vector<string> new_encoded_strings;
    const ColumnSchema& column = schema.column_by_id(hash_dimension.column_ids[col_offset]);
    const ColumnPredicate& predicate = FindOrDie(scan_spec.predicates(), column.name());
    const KeyEncoder<string>& encoder = GetKeyEncoder<string>(column.type_info());

    vector<const void*> predicate_values;
    if (predicate.predicate_type() == PredicateType::Equality) {
      predicate_values.push_back(predicate.raw_lower());
    } else {
      CHECK(predicate.predicate_type() == PredicateType::InList);
      predicate_values.insert(predicate_values.end(),
                              predicate.raw_values().begin(),
                              predicate.raw_values().end());
    }
    // For each of the encoded string, replicate it by the number of values in
    // equality and in-list predicate.
    for (const string& encoded_string : encoded_strings) {
      for (const void* predicate_value : predicate_values) {
        string new_encoded_string = encoded_string;
        encoder.Encode(predicate_value,
                       col_offset + 1 == hash_dimension.column_ids.size(),
                       &new_encoded_string);
        new_encoded_strings.emplace_back(new_encoded_string);
      }
    }
    encoded_strings.swap(new_encoded_strings);
  }
  for (const string& encoded_string : encoded_strings) {
    uint32_t hash_value = PartitionSchema::HashValueForEncodedColumns(
        encoded_string, hash_dimension);
    hash_bucket_bitset[hash_value] = true;
  }
  return hash_bucket_bitset;
}

vector<PartitionPruner::PartitionKeyRange> PartitionPruner::ConstructPartitionKeyRanges(
    const Schema& schema,
    const ScanSpec& scan_spec,
    const PartitionSchema::HashSchema& hash_schema,
    const RangeBounds& range_bounds) {
  // Create the hash bucket portion of the partition key.

  // The list of hash buckets bitset per hash component
  vector<vector<bool>> hash_bucket_bitsets;
  hash_bucket_bitsets.reserve(hash_schema.size());
  for (const auto& hash_dimension : hash_schema) {
    bool can_prune = true;
    for (const auto& column_id : hash_dimension.column_ids) {
      const ColumnSchema& column = schema.column_by_id(column_id);
      const ColumnPredicate* predicate = FindOrNull(scan_spec.predicates(), column.name());
      if (predicate == nullptr ||
          (predicate->predicate_type() != PredicateType::Equality &&
              predicate->predicate_type() != PredicateType::InList)) {
        can_prune = false;
        break;
      }
    }
    if (can_prune) {
      auto hash_bucket_bitset = PruneHashComponent(
          hash_dimension, schema, scan_spec);
      hash_bucket_bitsets.emplace_back(std::move(hash_bucket_bitset));
    } else {
      hash_bucket_bitsets.emplace_back(hash_dimension.num_buckets, true);
    }
  }

  // The index of the final constrained component in the partition key.
  size_t constrained_index;
  if (!range_bounds.lower.empty() || !range_bounds.upper.empty()) {
    // The range component is constrained.
    constrained_index = hash_schema.size();
  } else {
    // Search the hash bucket constraints from right to left, looking for the
    // first constrained component.
    constrained_index = hash_schema.size() -
        distance(hash_bucket_bitsets.rbegin(),
                 find_if(hash_bucket_bitsets.rbegin(),
                         hash_bucket_bitsets.rend(),
                         [] (const vector<bool>& x) {
                           return std::find(x.begin(), x.end(), false) != x.end();
                         }));
  }

  // Build up a set of partition key ranges out of the hash components.
  //
  // Each hash component simply appends its bucket number to the
  // partition key ranges (possibly incrementing the upper bound by one bucket
  // number if this is the final constraint, see note 2 in the example above).
  vector<PartitionKeyRange> partition_key_ranges(1);
  const KeyEncoder<string>& hash_encoder = GetKeyEncoder<string>(GetTypeInfo(UINT32));
  for (size_t hash_idx = 0; hash_idx < constrained_index; ++hash_idx) {
    // This is the final partition key component if this is the final constrained
    // bucket, and the range upper bound is empty. In this case we need to
    // increment the bucket on the upper bound to convert from inclusive to
    // exclusive.
    bool is_last = hash_idx + 1 == constrained_index && range_bounds.upper.empty();

    vector<PartitionKeyRange> new_partition_key_ranges;
    for (const auto& partition_key_range : partition_key_ranges) {
      const vector<bool>& buckets_bitset = hash_bucket_bitsets[hash_idx];
      for (uint32_t bucket = 0; bucket < buckets_bitset.size(); ++bucket) {
        if (!buckets_bitset[bucket]) {
          continue;
        }
        uint32_t bucket_upper = is_last ? bucket + 1 : bucket;
        string lower = partition_key_range.start;
        string upper = partition_key_range.end;
        hash_encoder.Encode(&bucket, &lower);
        hash_encoder.Encode(&bucket_upper, &upper);
        new_partition_key_ranges.emplace_back(PartitionKeyRange{move(lower), move(upper)});
      }
    }
    partition_key_ranges.swap(new_partition_key_ranges);
  }

  // Append the (possibly empty) range bounds to the partition key ranges.
  for (auto& range : partition_key_ranges) {
    range.start.append(range_bounds.lower);
    range.end.append(range_bounds.upper);
  }

  // Remove all partition key ranges past the scan spec's upper bound partition key.
  if (!scan_spec.exclusive_upper_bound_partition_key().empty()) {
    for (auto range = partition_key_ranges.rbegin();
         range != partition_key_ranges.rend();
         ++range) {
      if (!(*range).end.empty() &&
          scan_spec.exclusive_upper_bound_partition_key() >= (*range).end) {
        break;
      }
      if (scan_spec.exclusive_upper_bound_partition_key() <= (*range).start) {
        partition_key_ranges.pop_back();
      } else {
        (*range).end = scan_spec.exclusive_upper_bound_partition_key();
      }
    }
  }

  return partition_key_ranges;
}

void PartitionPruner::Init(const Schema& schema,
                           const PartitionSchema& partition_schema,
                           const ScanSpec& scan_spec) {
  // If we can already short circuit the scan, we don't need to bother with
  // partition pruning. This also allows us to assume some invariants of the
  // scan spec, such as no None predicates and that the lower bound PK < upper
  // bound PK.
  if (scan_spec.CanShortCircuit()) { return; }

  // Build a set of partition key ranges which cover the tablets necessary for
  // the scan.
  //
  // Example predicate sets and resulting partition key ranges, based on the
  // following tablet schema:
  //
  // CREATE TABLE t (a INT32, b INT32, c INT32) PRIMARY KEY (a, b, c)
  // DISTRIBUTE BY RANGE (c)
  //               HASH (a) INTO 2 BUCKETS
  //               HASH (b) INTO 3 BUCKETS;
  //
  // Assume that hash(0) = 0 and hash(2) = 2.
  //
  // | Predicates | Partition Key Ranges                                   |
  // +------------+--------------------------------------------------------+
  // | a = 0      | [(bucket=0, bucket=2, c=0), (bucket=0, bucket=2, c=1)) |
  // | b = 2      |                                                        |
  // | c = 0      |                                                        |
  // +------------+--------------------------------------------------------+
  // | a = 0      | [(bucket=0, bucket=2), (bucket=0, bucket=3))           |
  // | b = 2      |                                                        |
  // +------------+--------------------------------------------------------+
  // | a = 0      | [(bucket=0, bucket=0, c=0), (bucket=0, bucket=0, c=1)) |
  // | c = 0      | [(bucket=0, bucket=1, c=0), (bucket=0, bucket=1, c=1)) |
  // |            | [(bucket=0, bucket=2, c=0), (bucket=0, bucket=2, c=1)) |
  // +------------+--------------------------------------------------------+
  // | b = 2      | [(bucket=0, bucket=2, c=0), (bucket=0, bucket=2, c=1)) |
  // | c = 0      | [(bucket=1, bucket=2, c=0), (bucket=1, bucket=2, c=1)) |
  // +------------+--------------------------------------------------------+
  // | a = 0      | [(bucket=0), (bucket=1))                               |
  // +------------+--------------------------------------------------------+
  // | b = 2      | [(bucket=0, bucket=2), (bucket=0, bucket=3))           |
  // |            | [(bucket=1, bucket=2), (bucket=1, bucket=3))           |
  // +------------+--------------------------------------------------------+
  // | c = 0      | [(bucket=0, bucket=0, c=0), (bucket=0, bucket=0, c=1)) |
  // |            | [(bucket=0, bucket=1, c=0), (bucket=0, bucket=1, c=1)) |
  // |            | [(bucket=0, bucket=2, c=0), (bucket=0, bucket=2, c=1)) |
  // |            | [(bucket=1, bucket=0, c=0), (bucket=1, bucket=0, c=1)) |
  // |            | [(bucket=1, bucket=1, c=0), (bucket=1, bucket=1, c=1)) |
  // |            | [(bucket=1, bucket=2, c=0), (bucket=1, bucket=2, c=1)) |
  // +------------+--------------------------------------------------------+
  // | None       | [(), ())                                               |
  //
  // If the partition key is considered as a sequence of the hash bucket
  // components and a range component, then a few patterns emerge from the
  // examples above:
  //
  // 1) The partition keys are truncated after the final constrained component.
  //    Hash bucket components are constrained when the scan is limited to a
  //    subset of buckets via equality or in-list predicates on that component.
  //    Range components are constrained if they have an upper or lower bound
  //    via range or equality predicates on that component.
  //
  // 2) If the final constrained component is a hash bucket, then the
  //    corresponding bucket in the upper bound is incremented in order to make
  //    it an exclusive key.
  //
  // 3) The number of partition key ranges in the result is equal to the product
  //    of the number of buckets of each unconstrained hash component which come
  //    before a final constrained component. If there are no unconstrained hash
  //    components, then the number of resulting partition key ranges is one. Note
  //    that this can be a lot of ranges, and we may find we need to limit the
  //    algorithm to give up on pruning if the number of ranges exceeds a limit.
  //    Until this becomes a problem in practice, we'll continue always pruning,
  //    since it is precisely these highly-hash-partitioned tables which get the
  //    most benefit from pruning.

  // Build the range portion of the partition key by using
  // the lower and upper bounds specified by the scan.
  string scan_range_lower_bound;
  string scan_range_upper_bound;
  const vector<ColumnId>& range_columns = partition_schema.range_schema_.column_ids;
  if (!range_columns.empty()) {
    if (AreRangeColumnsPrefixOfPrimaryKey(schema, range_columns)) {
      EncodeRangeKeysFromPrimaryKeyBounds(schema,
                                          scan_spec,
                                          range_columns.size(),
                                          &scan_range_lower_bound,
                                          &scan_range_upper_bound);
    } else {
      EncodeRangeKeysFromPredicates(schema,
                                    scan_spec.predicates(),
                                    range_columns,
                                    &scan_range_lower_bound,
                                    &scan_range_upper_bound);
    }
  }

  // Store ranges and their corresponding hash schemas if they fall within
  // the range bounds specified by the scan.
  if (partition_schema.ranges_with_hash_schemas_.empty()) {
    auto partition_key_ranges = ConstructPartitionKeyRanges(
        schema, scan_spec, partition_schema.hash_schema_,
        {scan_range_lower_bound, scan_range_upper_bound});
    // Reverse the order of the partition key ranges, so that it is efficient
    // to remove the partition key ranges from the vector in ascending order.
    range_bounds_to_partition_key_ranges_.resize(1);
    auto& first_range = range_bounds_to_partition_key_ranges_[0];
    first_range.partition_key_ranges.resize(partition_key_ranges.size());
    move(partition_key_ranges.rbegin(), partition_key_ranges.rend(),
         first_range.partition_key_ranges.begin());
  } else {
    vector<RangeBounds> range_bounds;
    vector<PartitionSchema::HashSchema> hash_schemas_per_range;
    for (const auto& range : partition_schema.ranges_with_hash_schemas_) {
      const auto& hash_schema = range.hash_schema;
      // Both lower and upper bounds of the scan are unbounded.
      if (scan_range_lower_bound.empty() && scan_range_upper_bound.empty()) {
        range_bounds.emplace_back(RangeBounds{range.lower, range.upper});
        hash_schemas_per_range.emplace_back(hash_schema);
        continue;
      }
      // Only one of the lower/upper bounds of the scan is unbounded.
      if (scan_range_lower_bound.empty()) {
        if (scan_range_upper_bound > range.lower) {
          range_bounds.emplace_back(RangeBounds{range.lower, range.upper});
          hash_schemas_per_range.emplace_back(hash_schema);
        }
        continue;
      }
      if (scan_range_upper_bound.empty()) {
        if (range.upper.empty() || scan_range_lower_bound < range.upper) {
          range_bounds.emplace_back(RangeBounds{range.lower, range.upper});
          hash_schemas_per_range.emplace_back(hash_schema);
        }
        continue;
      }
      // Both lower and upper ranges of the scan are bounded.
      if ((range.upper.empty() || scan_range_lower_bound < range.upper) &&
          scan_range_upper_bound > range.lower) {
        range_bounds.emplace_back(RangeBounds{range.lower, range.upper});
        hash_schemas_per_range.emplace_back(hash_schema);
      }
    }
    DCHECK_EQ(range_bounds.size(), hash_schemas_per_range.size());
    range_bounds_to_partition_key_ranges_.resize(hash_schemas_per_range.size());
    // Construct partition key ranges from the ranges and their respective hash schemas
    // that falls within the scan's bounds.
    for (size_t i = 0; i < hash_schemas_per_range.size(); ++i) {
      const auto& hash_schema = hash_schemas_per_range[i];
      const auto bounds =
          scan_range_lower_bound.empty() && scan_range_upper_bound.empty()
          ? RangeBounds{range_bounds[i].lower, range_bounds[i].upper}
          : RangeBounds{scan_range_lower_bound, scan_range_upper_bound};
      auto partition_key_ranges = ConstructPartitionKeyRanges(
          schema, scan_spec, hash_schema, bounds);
      auto& current_range = range_bounds_to_partition_key_ranges_[i];
      current_range.range_bounds = range_bounds[i];
      current_range.partition_key_ranges.resize(partition_key_ranges.size());
      move(partition_key_ranges.rbegin(), partition_key_ranges.rend(),
           current_range.partition_key_ranges.begin());
    }
  }

  // Remove all partition key ranges before the scan spec's lower bound partition key.
  if (!scan_spec.lower_bound_partition_key().empty()) {
    RemovePartitionKeyRange(scan_spec.lower_bound_partition_key());
  }

}
bool PartitionPruner::HasMorePartitionKeyRanges() const {
  return NumRangesRemaining() != 0;
}

const string& PartitionPruner::NextPartitionKey() const {
  CHECK(HasMorePartitionKeyRanges());
  return range_bounds_to_partition_key_ranges_.back().partition_key_ranges.back().start;
}

void PartitionPruner::RemovePartitionKeyRange(const string& upper_bound) {
  if (upper_bound.empty()) {
    range_bounds_to_partition_key_ranges_.clear();
    return;
  }

  for (auto& range_bounds_and_partition_key_range : range_bounds_to_partition_key_ranges_) {
    auto& partition_key_range = range_bounds_and_partition_key_range.partition_key_ranges;
    for (auto range_it = partition_key_range.rbegin();
         range_it != partition_key_range.rend();
         ++range_it) {
      if (upper_bound <= (*range_it).start) { break; }
      if ((*range_it).end.empty() || upper_bound < (*range_it).end) {
        (*range_it).start = upper_bound;
      } else {
        partition_key_range.pop_back();
      }
    }
  }
}

bool PartitionPruner::ShouldPrune(const Partition& partition) const {
  for (const auto& [range_bounds, partition_key_range] : range_bounds_to_partition_key_ranges_) {
    // Check if the partition belongs to the same range as the partition key range.
    if (!range_bounds.lower.empty() && partition.range_key_start() != range_bounds.lower &&
        !range_bounds.upper.empty() && partition.range_key_end() != range_bounds.upper) {
      continue;
    }
    // range is an iterator that points to the first partition key range which
    // overlaps or is greater than the partition.
    auto range = lower_bound(partition_key_range.rbegin(), partition_key_range.rend(),
                             partition, [] (const PartitionKeyRange& scan_range,
                                 const Partition& partition) {
                               // return true if scan_range < partition
                               const string& scan_upper = scan_range.end;
                               return !scan_upper.empty()
                               && scan_upper <= partition.partition_key_start();
                             });
    if (!(range == partition_key_range.rend() ||
       (!partition.partition_key_end().empty() &&
       partition.partition_key_end() <= (*range).start))) {
      return false;
    }
  }
  return true;
}

string PartitionPruner::ToString(const Schema& schema,
                                 const PartitionSchema& partition_schema) const {
  vector<string> strings;
  for (const auto& partition_key_range : range_bounds_to_partition_key_ranges_) {
    for (auto range = partition_key_range.partition_key_ranges.rbegin();
         range != partition_key_range.partition_key_ranges.rend();
         ++range) {
      strings.push_back(strings::Substitute(
          "[($0), ($1))",
          (*range).start.empty() ? "<start>" :
            partition_schema.PartitionKeyDebugString((*range).start, schema),
          (*range).end.empty() ? "<end>" :
            partition_schema.PartitionKeyDebugString((*range).end, schema)));
    }
  }

  return JoinStrings(strings, ", ");
}

} // namespace kudu
