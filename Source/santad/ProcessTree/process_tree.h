/// Copyright 2023 Google LLC
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
#ifndef SANTA__SANTAD_PROCESSTREE_TREE_H
#define SANTA__SANTAD_PROCESSTREE_TREE_H

#include <memory>
#include <typeinfo>
#include <vector>

#include "Source/santad/ProcessTree/process.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"

namespace santa::santad::process_tree {

absl::StatusOr<Process> LoadPID(pid_t pid);

// Fwd decl for test peer.
class ProcessTreeTestPeer;

class ProcessTree {
 public:
  explicit ProcessTree(std::vector<std::unique_ptr<Annotator>> &&annotators)
      : annotators_(std::move(annotators)), seen_timestamps_({}) {}
  ProcessTree(const ProcessTree &) = delete;
  ProcessTree &operator=(const ProcessTree &) = delete;
  ProcessTree(ProcessTree &&) = delete;
  ProcessTree &operator=(ProcessTree &&) = delete;

  // Initialize the tree with the processes currently running on the system.
  absl::Status Backfill();

  // Inform the tree of a fork event, in which the parent process spawns a child
  // with the only difference between the two being the pid.
  void HandleFork(uint64_t timestamp, const Process &parent,
                  struct Pid new_pid);

  // Inform the tree of an exec event, in which the program and potentially cred
  // of a Process change.
  // p is the process performing the exec (running the "old" program),
  // and new_pid, prog, and cred are the new pid, program, and credentials
  // after the exec.
  // N.B. new_pid is required as the "pid version" will have changed.
  // It is a programming error to pass a new_pid such that
  // p.pid_.pid != new_pid.pid.
  void HandleExec(uint64_t timestamp, const Process &p, struct Pid new_pid,
                  struct Program prog, struct Cred c);

  // Inform the tree of a process exit.
  void HandleExit(uint64_t timestamp, const Process &p);

  // Mark the given pids as needing to be retained in the tree's map for future
  // access. Normally, Processes are removed once all clients process past the
  // event which would remove the Process (e.g. exit), however in cases where
  // async processing occurs, the Process may need to be accessed after the
  // exit.
  void RetainProcess(std::vector<struct Pid> &pids);

  // Release previously retained processes, signaling that the client is done
  // processing the event that retained them.
  void ReleaseProcess(std::vector<struct Pid> &pids);

  // Annotate the given process with an Annotator (state).
  void AnnotateProcess(const Process &p, std::shared_ptr<const Annotator> a);

  // Get the given annotation on the given process if it exists, or nullopt if
  // the annotation is not set.
  template <typename T>
  std::optional<std::shared_ptr<const T>> GetAnnotation(const Process &p) const;

  // Get the fully merged proto form of all annotations on the given process.
  std::optional<::santa::pb::v1::process_tree::Annotations> ExportAnnotations(
      struct Pid p);

  // Atomically get the slice of Processes going from the given process "up"
  // to the root. The root process has no parent. N.B. There may be more than
  // one root process. E.g. on Linux, both init (PID 1) and kthread (PID 2)
  // are considered roots, as they are reported to have PPID=0.
  std::vector<std::shared_ptr<const Process>> RootSlice(
      std::shared_ptr<const Process> p) const;

  // Call f for all processes in the tree. The list of processes is captured
  // before invoking f, so it is safe to mutate the tree in f.
  void Iterate(std::function<void(std::shared_ptr<const Process>)> f) const;

  // Get the Process for the given pid in the tree if it exists.
  std::optional<std::shared_ptr<const Process>> Get(struct Pid target) const;

  // Traverse the tree from the given Process to its parent.
  std::shared_ptr<const Process> GetParent(const Process &p) const;

#if SANTA_PROCESS_TREE_DEBUG
  // Dump the tree in a human readable form to the given ostream.
  void DebugDump(std::ostream &stream) const;
#endif

 private:
  friend class ProcessTreeTestPeer;
  void BackfillInsertChildren(
      absl::flat_hash_map<pid_t, std::vector<Process>> &parent_map,
      std::shared_ptr<Process> parent, const Process &unlinked_proc);

  // Mark that an event with the given timestamp is being processed.
  // Returns whether the given timestamp is "novel", and the tree should be
  // updated with the results of the event.
  bool Step(uint64_t timestamp);

  std::optional<std::shared_ptr<Process>> GetLocked(struct Pid target) const
      ABSL_SHARED_LOCKS_REQUIRED(mtx_);

  void DebugDumpLocked(std::ostream &stream, int depth, pid_t ppid) const;

  std::vector<std::unique_ptr<Annotator>> annotators_;

  mutable absl::Mutex mtx_;
  absl::flat_hash_map<const struct Pid, std::shared_ptr<Process>> map_
      ABSL_GUARDED_BY(mtx_);
  // List of pids which should be removed from map_, and at the timestamp at
  // which they should be.
  // Elements are removed when the timestamp falls out of the seen_timestamps_
  // list below, signifying that all clients have synced past the timestamp.
  std::vector<std::pair<uint64_t, struct Pid>> remove_at_ ABSL_GUARDED_BY(mtx_);
  // Rolling list of event timestamps processed by the tree.
  // This is used to ensure an event only gets processed once, even if events
  // come out of order.
  std::array<uint64_t, 32> seen_timestamps_ ABSL_GUARDED_BY(mtx_);
};

template <typename T>
std::optional<std::shared_ptr<const T>> ProcessTree::GetAnnotation(
    const Process &p) const {
  auto it = p.annotations_.find(std::type_index(typeid(T)));
  if (it == p.annotations_.end()) {
    return std::nullopt;
  }
  return std::dynamic_pointer_cast<const T>(it->second);
}

// Create a new tree, ensuring the provided annotations are valid and that
// backfill is successful.
absl::StatusOr<std::shared_ptr<ProcessTree>> CreateTree(
    std::vector<std::unique_ptr<Annotator>> &&annotations);

// ProcessTokens provide a lifetime based approach to retaining processes
// in a ProcessTree. When a token is created with a list of pids that may need
// to be referenced during processing of a given event, the ProcessToken informs
// the tree to retain those pids in its map so any call to ProcessTree::Get()
// during event processing succeeds. When the token is destroyed, it signals the
// tree to release the pids, which removes them from the tree if they would have
// fallen out otherwise due to a destruction event (e.g. exit).
class ProcessToken {
 public:
  explicit ProcessToken(std::shared_ptr<ProcessTree> tree,
                        std::vector<struct Pid> pids);
  ~ProcessToken();
  ProcessToken(const ProcessToken &other)
      : ProcessToken(other.tree_, other.pids_) {}
  ProcessToken(ProcessToken &&other) noexcept
      : tree_(std::move(other.tree_)), pids_(std::move(other.pids_)) {}
  ProcessToken &operator=(const ProcessToken &other) {
    return *this = ProcessToken(other.tree_, other.pids_);
  }
  ProcessToken &operator=(ProcessToken &&other) noexcept {
    tree_ = std::move(other.tree_);
    pids_ = std::move(other.pids_);
    return *this;
  }

 private:
  std::shared_ptr<ProcessTree> tree_;
  std::vector<struct Pid> pids_;
};

}  // namespace santa::santad::process_tree

#endif
