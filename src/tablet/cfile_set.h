// Copyright (c) 2013, Cloudera, inc.
#ifndef KUDU_TABLET_LAYER_BASEDATA_H
#define KUDU_TABLET_LAYER_BASEDATA_H

#include <gtest/gtest.h>
#include <tr1/memory>
#include <string>
#include <vector>

#include "cfile/bloomfile.h"
#include "cfile/cfile_reader.h"

#include "common/iterator.h"
#include "common/schema.h"
#include "gutil/macros.h"
#include "server/metadata.h"
#include "tablet/memrowset.h"
#include "util/env.h"
#include "util/memory/arena.h"
#include "util/slice.h"

namespace kudu {

namespace metadata {
class RowSetMetadata;
}

namespace tablet {

using kudu::cfile::BloomFileReader;
using kudu::cfile::CFileIterator;
using kudu::cfile::CFileReader;
using kudu::cfile::ColumnIterator;
using std::tr1::shared_ptr;

// Set of CFiles which make up the base data for a single rowset
//
// All of these files have the same number of rows, and thus the positional
// indexes can be used to seek to corresponding entries in each.
class CFileSet : public std::tr1::enable_shared_from_this<CFileSet> {
 public:
  class Iterator;

  explicit CFileSet(const shared_ptr<metadata::RowSetMetadata>& rowset_metadata);

  Status Open();

  // Create an iterator with the given projection. 'projection' must remain valid
  // for the lifetime of the returned iterator.
  virtual Iterator *NewIterator(const Schema *projection) const;

  Status CountRows(rowid_t *count) const;

  // See RowSet::GetBounds
  virtual Status GetBounds(Slice *min_encoded_key,
                           Slice *max_encoded_key) const;

  uint64_t EstimateOnDiskSize() const;

  // Determine the index of the given row key.
  Status FindRow(const RowSetKeyProbe &probe, rowid_t *idx, ProbeStats* stats) const;

  const Schema &schema() const { return rowset_metadata_->schema(); }

  string ToString() const {
    return string("CFile base data in ") + rowset_metadata_->ToString();
  }

  // Check if the given row is present. If it is, sets *rowid to the
  // row's index.
  Status CheckRowPresent(const RowSetKeyProbe &probe, bool *present,
                         rowid_t *rowid, ProbeStats* stats) const;

  virtual ~CFileSet();

 private:
  friend class Iterator;
  friend class CFileSetIteratorProjector;

  DISALLOW_COPY_AND_ASSIGN(CFileSet);

  Status OpenBloomReader();
  Status OpenAdHocIndexReader();
  Status LoadMinMaxKeys();

  Status NewColumnIterator(size_t col_idx, CFileIterator **iter) const;
  Status NewAdHocIndexIterator(CFileIterator **iter) const;

  Status NewKeyIterator(CFileIterator **iter) const;

  // Return the CFileReader responsible for reading the key index.
  // (the ad-hoc reader for composite keys, otherwise the key column reader)
  CFileReader *key_index_reader();

  shared_ptr<metadata::RowSetMetadata> rowset_metadata_;

  std::string min_encoded_key_;
  std::string max_encoded_key_;

  vector<shared_ptr<CFileReader> > readers_;

  // A file reader for an ad-hoc index, i.e. an index that sits in its own file
  // and is not embedded with the column's data blocks. This is used when the
  // index pertains to more than one column, as in the case of composite keys.
  gscoped_ptr<CFileReader> ad_hoc_idx_reader_;
  gscoped_ptr<BloomFileReader> bloom_reader_;
};


////////////////////////////////////////////////////////////

// Column-wise iterator implementation over a set of column files.
//
// This simply ties together underlying files so that they can be batched
// together, and iterated in parallel.
class CFileSet::Iterator : public ColumnwiseIterator {
 public:

  virtual Status Init(ScanSpec *spec) OVERRIDE;

  virtual Status PrepareBatch(size_t *nrows) OVERRIDE;

  virtual Status InitializeSelectionVector(SelectionVector *sel_vec) OVERRIDE;

  virtual Status MaterializeColumn(size_t col_idx, ColumnBlock *dst) OVERRIDE;

  virtual Status FinishBatch() OVERRIDE;

  virtual bool HasNext() const OVERRIDE {
    DCHECK(initted_);
    return cur_idx_ <= upper_bound_idx_;
  }

  virtual string ToString() const OVERRIDE {
    return string("rowset iterator for ") + base_data_->ToString();
  }

  const Schema &schema() const OVERRIDE {
    return *projection_;
  }

  // Collect the IO statistics for each of the underlying columns.
  virtual void GetIteratorStats(vector<IteratorStats> *stats) const OVERRIDE;

  virtual ~Iterator();
 private:
  DISALLOW_COPY_AND_ASSIGN(Iterator);
  FRIEND_TEST(TestCFileSet, TestRangeScan);
  friend class CFileSet;

  // 'projection' must remain valid for the lifetime of this object.
  Iterator(const shared_ptr<CFileSet const> &base_data,
           const Schema *projection)
    : base_data_(base_data),
      projection_(projection),
      initted_(false),
      prepared_count_(0) {
    CHECK_OK(base_data_->CountRows(&row_count_));
  }


  // Look for a predicate which can be converted into a range scan using the key
  // column's index. If such a predicate exists, remove it from the scan spec and
  // store it in member fields.
  Status PushdownRangeScanPredicate(ScanSpec *spec);

  Status SeekToOrdinal(rowid_t ord_idx);
  void Unprepare();

  // Prepare the given column if not already prepared.
  Status PrepareColumn(size_t col_idx);

  const shared_ptr<CFileSet const> base_data_;
  const Schema* projection_;

  // Iterator for the key column in the underlying data.
  gscoped_ptr<CFileIterator> key_iter_;
  std::vector<ColumnIterator*> col_iters_;

  bool initted_;

  size_t cur_idx_;
  size_t prepared_count_;

  // The total number of rows in the file
  rowid_t row_count_;

  // Upper and lower (inclusive) bounds for this iterator, in terms of ordinal row indexes.
  // Both of these bounds are _inclusive_, and are always set (even if there is no predicate).
  // If there is no predicate, then the bounds will be [0, row_count_-1]
  rowid_t lower_bound_idx_;
  rowid_t upper_bound_idx_;


  // The underlying columns are prepared lazily, so that if a column is never
  // materialized, it doesn't need to be read off disk.
  vector<bool> cols_prepared_;

};

} // namespace tablet
} // namespace kudu
#endif
