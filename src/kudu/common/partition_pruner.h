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

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "kudu/common/partition.h"
#include "kudu/gutil/macros.h"

namespace kudu {

class ScanSpec;
class Schema;

// Provides partition key ranges to a scanner in order to prune tablets which
// are not necessary for the scan. The scanner retrieves the partition key of
// the next tablet to scan through the NextPartitionKey method, and notifies the
// partition pruner that a tablet has been scanned by calling
// RemovePartitionKeyRange with the tablet's upper bound partition key.
//
// Partition keys are in the same encoded format as used by the Partition class.
class PartitionPruner {
 public:
  PartitionPruner() = default;

  // Initializes the partition pruner for a new scan. The scan spec should
  // already be optimized by the ScanSpec::Optimize method.
  void Init(const Schema& schema,
            const PartitionSchema& partition_schema,
            const ScanSpec& scan_spec);

  // Returns whether there are more partition key ranges to scan.
  bool HasMorePartitionKeyRanges() const;

  // Returns the inclusive lower bound partition key of the next tablet to scan.
  const std::string& NextPartitionKey() const;

  // Removes all partition key ranges through the provided exclusive upper bound.
  void RemovePartitionKeyRange(const std::string& upper_bound);

  // Returns true if the provided partition should be pruned.
  bool ShouldPrune(const Partition& partition) const;

  // Returns the number of partition key ranges remaining in the scan.
  size_t NumRangesRemaining() const {
    size_t num_ranges = 0;
    for (const auto& range: range_bounds_to_partition_key_ranges_) {
      num_ranges += range.partition_key_ranges.size();
    }
    return num_ranges;
  }

  // Returns a text description of this partition pruner suitable for debug
  // printing.
  std::string ToString(const Schema& schema, const PartitionSchema& partition_schema) const;

 private:
  struct RangeBounds {
    std::string lower;
    std::string upper;
  };

  struct PartitionKeyRange {
    std::string start;
    std::string end;
  };

  struct RangeBoundsAndPartitionKeyRanges {
    RangeBounds range_bounds;
    std::vector<PartitionKeyRange> partition_key_ranges;
  };

  // Search all combinations of in-list and equality predicates.
  // Return hash values bitset of these combinations.
  static std::vector<bool> PruneHashComponent(
      const PartitionSchema::HashDimension& hash_dimension,
      const Schema& schema,
      const ScanSpec& scan_spec);

  // Given the range bounds and the hash schema, constructs a set of partition
  // key ranges.
  static std::vector<PartitionKeyRange> ConstructPartitionKeyRanges(
      const Schema& schema,
      const ScanSpec& scan_spec,
      const PartitionSchema::HashSchema& hash_schema,
      const RangeBounds& range_bounds);

  // A vector of a pair of lower and upper range bounds mapped to a
  // reverse sorted set of partition key ranges. Each partition key range within the set
  // has an inclusive lower bound and an exclusive upper bound.
  std::vector<RangeBoundsAndPartitionKeyRanges> range_bounds_to_partition_key_ranges_;

  DISALLOW_COPY_AND_ASSIGN(PartitionPruner);
};

} // namespace kudu
