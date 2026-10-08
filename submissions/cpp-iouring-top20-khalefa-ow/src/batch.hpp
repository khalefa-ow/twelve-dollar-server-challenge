#pragma once
#include "api.hpp"
#include "wal.hpp"

namespace top20 {

struct BatchResult { int status = 0; std::string body; };
class WriteBatch {
 public:
  Store candidate;
  std::vector<BatchResult> results;
  std::string operations, frame;
  // Exceptions discard the whole candidate. Nothing has been published or acknowledged.
  void prepare(const Store& committed, Journal& journal, std::vector<Mutation>& writes) {
    candidate = committed;
    results.clear();
    results.reserve(writes.size());
    operations.clear();
    frame.clear();
    for (auto& m : writes) {
      BatchResult result;
      result.status = apply_mutation(candidate, m, result.body);
      if (result.status == 201) {
        if (m.kind == Mutation::Kind::Create) disk::append_post(operations, *candidate.find(m.id));
        else disk::append_like(operations, m.id, m.user, candidate.find(m.id)->likes.size());
      }
      results.push_back(std::move(result));
    }
    if (!operations.empty()) frame = journal.prepare(operations);
  }
  void publish(Store& committed, Journal& journal) {
    if (!frame.empty()) journal.published(frame.size());
    committed = std::move(candidate);
  }
};

}  // namespace top20
