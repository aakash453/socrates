#include "socrates/persistence/assignment_store.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace socrates::persistence {

namespace {

// ── Minimal JSON helpers (no library dependency) ──────────────────────────────

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

// Returns the substring between the next two '"' chars, handling \" escapes.
std::optional<std::string> json_extract_string(std::string_view json,
                                               std::string_view key) {
  std::string search = "\"";
  search += key;
  search += "\":\"";
  auto pos = json.find(search);
  if (pos == std::string_view::npos) return std::nullopt;
  pos += search.size();
  auto end = json.find('"', pos);
  if (end == std::string_view::npos) return std::nullopt;
  // Unescape
  std::string val;
  val.reserve(end - pos);
  for (auto i = pos; i < end; ++i) {
    if (json[i] == '\\' && i + 1 < end) {
      ++i;
      val += json[i];
    } else {
      val += json[i];
    }
  }
  return val;
}

// Returns the integer literal immediately following `"key":`, or nullopt.
std::optional<std::int64_t> json_extract_int(std::string_view json,
                                             std::string_view key) {
  std::string search = "\"";
  search += key;
  search += "\":";
  auto pos = json.find(search);
  if (pos == std::string_view::npos) return std::nullopt;
  pos += search.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  if (pos >= json.size()) return std::nullopt;
  char* end = nullptr;
  auto val = std::strtoll(json.data() + pos, &end, 10);
  if (end == json.data() + pos) return std::nullopt;
  return val;
}

std::string plan_to_json(const scheduler::PipelinePlan& plan) {
  auto issued_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          plan.issued_at_utc.time_since_epoch())
          .count();

  std::string json;
  json += "{\"plan_id\":\"";
  json += json_escape(plan.plan_id);
  json += "\",\"model_id\":\"";
  json += json_escape(plan.model_id.value);
  json += "\",\"manifest_revision\":\"";
  json += json_escape(plan.manifest_revision);
  json += "\",\"fence_term\":";
  json += std::to_string(plan.fence.term);
  json += ",\"fence_token\":\"";
  json += json_escape(plan.fence.token.value);
  json += "\",\"fence_membership_revision\":";
  json += std::to_string(plan.fence.membership_revision);
  json += ",\"issued_at_epoch_ms\":";
  json += std::to_string(issued_ms);
  json += ",\"valid_for_ms\":";
  json += std::to_string(plan.valid_for.count());
  json += "}";
  return json;
}

scheduler::PipelinePlan plan_from_json(std::string_view json) {
  scheduler::PipelinePlan plan;

  if (auto v = json_extract_string(json, "plan_id"))
    plan.plan_id = std::move(*v);
  if (auto v = json_extract_string(json, "model_id"))
    plan.model_id.value = std::move(*v);
  if (auto v = json_extract_string(json, "manifest_revision"))
    plan.manifest_revision = std::move(*v);
  if (auto v = json_extract_int(json, "fence_term"))
    plan.fence.term = static_cast<std::uint64_t>(*v);
  if (auto v = json_extract_string(json, "fence_token"))
    plan.fence.token.value = std::move(*v);
  if (auto v = json_extract_int(json, "fence_membership_revision"))
    plan.fence.membership_revision = static_cast<std::uint64_t>(*v);
  if (auto v = json_extract_int(json, "issued_at_epoch_ms"))
    plan.issued_at_utc = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(*v));
  if (auto v = json_extract_int(json, "valid_for_ms"))
    plan.valid_for = std::chrono::milliseconds(*v);

  return plan;
}

// ── RAII prepared-statement wrapper ───────────────────────────────────────────

class Statement final {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) stmt_ = nullptr;
  }

  ~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&&) = delete;
  Statement& operator=(Statement&&) = delete;

  [[nodiscard]] bool valid() const { return stmt_ != nullptr; }
  [[nodiscard]] sqlite3_stmt* get() const { return stmt_; }

  bool step() { return sqlite3_step(stmt_) == SQLITE_ROW; }
  int step_result() { return sqlite3_step(stmt_); }

  void bind_text(int idx, std::string_view s) {
    sqlite3_bind_text(stmt_, idx, s.data(), static_cast<int>(s.size()),
                      SQLITE_TRANSIENT);
  }
  void bind_int64(int idx, std::int64_t v) {
    sqlite3_bind_int64(stmt_, idx, v);
  }
  void bind_int(int idx, int v) { sqlite3_bind_int(stmt_, idx, v); }
  void bind_double(int idx, double v) { sqlite3_bind_double(stmt_, idx, v); }

  std::string_view column_text(int idx) {
    const char* p =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt_, idx));
    int n = sqlite3_column_bytes(stmt_, idx);
    return p ? std::string_view(p, static_cast<std::size_t>(n)) : "";
  }
  std::int64_t column_int64(int idx) {
    return sqlite3_column_int64(stmt_, idx);
  }
  int column_int(int idx) { return sqlite3_column_int(stmt_, idx); }
  double column_double(int idx) { return sqlite3_column_double(stmt_, idx); }

 private:
  sqlite3* db_;
  sqlite3_stmt* stmt_{nullptr};
};

}  // namespace

// ── SqliteAssignmentStore ─────────────────────────────────────────────────────

class SqliteAssignmentStore final : public AssignmentStore {
 public:
  explicit SqliteAssignmentStore(std::string_view db_path)
      : db_path_(db_path) {
    int rc = sqlite3_open_v2(
        db_path_.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc == SQLITE_OK) {
      create_tables();
    } else {
      db_ = nullptr;
    }
  }

  ~SqliteAssignmentStore() override {
    if (db_) sqlite3_close(db_);
  }

  // ── save_plan ───────────────────────────────────────────────────────────────

  Result<bool> save_plan(const scheduler::PipelinePlan& plan) override {
    std::lock_guard lock(mutex_);
    if (!db_) return errmsg("database not open");

    std::string json = plan_to_json(plan);

    if (!exec("BEGIN TRANSACTION")) return errmsg("BEGIN TRANSACTION");

    // INSERT OR REPLACE the plan row
    {
      Statement stmt(db_,
                     "INSERT OR REPLACE INTO plans "
                     "(plan_id, model_id, manifest_revision, fence_term, "
                     "fence_token, fence_membership_revision, "
                     "issued_at_epoch_ms, valid_for_ms, plan_json) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
      if (!stmt.valid()) {
        rollback();
        return errmsg();
      }
      stmt.bind_text(1, plan.plan_id);
      stmt.bind_text(2, plan.model_id.value);
      stmt.bind_text(3, plan.manifest_revision);
      stmt.bind_int64(4, static_cast<std::int64_t>(plan.fence.term));
      stmt.bind_text(5, plan.fence.token.value);
      stmt.bind_int64(6,
                      static_cast<std::int64_t>(plan.fence.membership_revision));
      stmt.bind_int64(
          7, static_cast<std::int64_t>(
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     plan.issued_at_utc.time_since_epoch())
                     .count()));
      stmt.bind_int64(8, static_cast<std::int64_t>(plan.valid_for.count()));
      stmt.bind_text(9, json);
      if (stmt.step_result() != SQLITE_DONE) {
        rollback();
        return errmsg();
      }
    }

    // Delete old stages for this plan_id, then re-insert
    {
      Statement stmt(db_, "DELETE FROM stages WHERE plan_id = ?");
      if (!stmt.valid()) {
        rollback();
        return errmsg();
      }
      stmt.bind_text(1, plan.plan_id);
      if (stmt.step_result() != SQLITE_DONE) {
        rollback();
        return errmsg();
      }
    }

    for (const auto& s : plan.stages) {
      Statement stmt(db_,
                     "INSERT INTO stages "
                     "(plan_id, model_id, ordinal, node_id, shard_id, "
                     "backend_kind, quantization_kind, reserved_memory_bytes, "
                     "estimated_stage_microseconds) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
      if (!stmt.valid()) {
        rollback();
        return errmsg();
      }
      stmt.bind_text(1, plan.plan_id);
      stmt.bind_text(2, plan.model_id.value);
      stmt.bind_int(3, static_cast<int>(s.ordinal));
      stmt.bind_text(4, s.node_id.value);
      stmt.bind_text(5, s.shard_id.value);
      stmt.bind_int(6, static_cast<int>(s.backend));
      stmt.bind_int(7, static_cast<int>(s.quantization));
      stmt.bind_int64(8, static_cast<std::int64_t>(s.reserved_memory_bytes));
      stmt.bind_double(9, s.estimated_stage_microseconds);
      if (stmt.step_result() != SQLITE_DONE) {
        rollback();
        return errmsg();
      }
    }

    if (!exec("COMMIT")) {
      rollback();
      return errmsg("COMMIT");
    }

    return Result<bool>::Ok(true);
  }

  // ── load_plan ───────────────────────────────────────────────────────────────

  Result<std::optional<scheduler::PipelinePlan>> load_plan(
      const ModelId& model_id,
      const std::string& manifest_revision) const override {
    std::lock_guard lock(mutex_);
    if (!db_) return Result<std::optional<scheduler::PipelinePlan>>::Ok(
        std::nullopt);

    // Read plan_json
    std::string json;
    {
      Statement stmt(db_,
                     "SELECT plan_json FROM plans "
                     "WHERE model_id = ? AND manifest_revision = ? "
                     "ORDER BY fence_term DESC LIMIT 1");
      if (!stmt.valid())
        return Result<std::optional<scheduler::PipelinePlan>>::Err(
            ErrorCode::kInternal, sqlite3_errmsg(db_));
      stmt.bind_text(1, model_id.value);
      stmt.bind_text(2, manifest_revision);
      if (!stmt.step())
        return Result<std::optional<scheduler::PipelinePlan>>::Ok(std::nullopt);
      json = stmt.column_text(0);
    }

    auto plan = plan_from_json(json);

    // Load stages
    {
      Statement stmt(db_,
                     "SELECT ordinal, node_id, shard_id, backend_kind, "
                     "quantization_kind, reserved_memory_bytes, "
                     "estimated_stage_microseconds "
                     "FROM stages WHERE plan_id = ? ORDER BY ordinal ASC");
      if (!stmt.valid())
        return Result<std::optional<scheduler::PipelinePlan>>::Err(
            ErrorCode::kInternal, sqlite3_errmsg(db_));
      stmt.bind_text(1, plan.plan_id);
      while (stmt.step()) {
        scheduler::StageAssignment stage;
        stage.ordinal = static_cast<std::uint32_t>(stmt.column_int(0));
        stage.node_id.value = std::string(stmt.column_text(1));
        stage.shard_id.value = std::string(stmt.column_text(2));
        stage.backend = static_cast<BackendKind>(stmt.column_int(3));
        stage.quantization =
            static_cast<QuantizationKind>(stmt.column_int(4));
        stage.reserved_memory_bytes =
            static_cast<std::uint64_t>(stmt.column_int64(5));
        stage.estimated_stage_microseconds = stmt.column_double(6);
        plan.stages.push_back(std::move(stage));
      }
    }

    return Result<std::optional<scheduler::PipelinePlan>>::Ok(std::move(plan));
  }

  // ── append_token ────────────────────────────────────────────────────────────

  Result<bool> append_token(const TokenRecord& token) override {
    std::lock_guard lock(mutex_);
    if (!db_) return errmsg("database not open");

    // Validate contiguity
    {
      Statement stmt(db_,
                     "SELECT MAX(position) FROM tokens WHERE request_id = ?");
      if (!stmt.valid()) return errmsg();
      stmt.bind_text(1, token.request_id.value);
      if (stmt.step() &&
          sqlite3_column_type(stmt.get(), 0) != SQLITE_NULL) {
        auto max_pos =
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 0));
        if (max_pos + 1 != token.position)
          return Result<bool>::Err(ErrorCode::kDataLoss,
                                   "non-contiguous token position");
      }
    }

    Statement stmt(db_,
                   "INSERT OR IGNORE INTO tokens "
                   "(request_id, session_id, position, token_id) "
                   "VALUES (?, ?, ?, ?)");
    if (!stmt.valid()) return errmsg();
    stmt.bind_text(1, token.request_id.value);
    stmt.bind_text(2, token.session_id.value);
    stmt.bind_int64(3, static_cast<std::int64_t>(token.position));
    stmt.bind_int(4, token.token_id);
    if (stmt.step_result() != SQLITE_DONE) return errmsg();

    return Result<bool>::Ok(true);
  }

  // ── load_token_history ──────────────────────────────────────────────────────

  Result<std::vector<TokenRecord>> load_token_history(
      const RequestId& request_id,
      std::uint64_t start_position,
      std::uint32_t maximum_count) const override {
    std::lock_guard lock(mutex_);
    if (!db_) return Result<std::vector<TokenRecord>>::Ok({});

    Statement stmt(db_,
                   "SELECT position, token_id FROM tokens "
                   "WHERE request_id = ? AND position >= ? "
                   "ORDER BY position ASC LIMIT ?");
    if (!stmt.valid())
      return Result<std::vector<TokenRecord>>::Err(ErrorCode::kInternal,
                                                    sqlite3_errmsg(db_));
    stmt.bind_text(1, request_id.value);
    stmt.bind_int64(2, static_cast<std::int64_t>(start_position));
    stmt.bind_int(3, static_cast<int>(maximum_count));

    std::vector<TokenRecord> result;
    while (stmt.step()) {
      TokenRecord tr;
      tr.request_id = request_id;
      tr.session_id.value.clear();  // not stored in this query
      tr.position =
          static_cast<std::uint64_t>(stmt.column_int64(0));
      tr.token_id = static_cast<std::int32_t>(stmt.column_int(1));
      result.push_back(std::move(tr));
    }
    return Result<std::vector<TokenRecord>>::Ok(std::move(result));
  }

  // ── erase_token_history ─────────────────────────────────────────────────────

  Result<bool> erase_token_history(const RequestId& request_id) override {
    std::lock_guard lock(mutex_);
    if (!db_) return errmsg("database not open");

    Statement stmt(db_, "DELETE FROM tokens WHERE request_id = ?");
    if (!stmt.valid()) return errmsg();
    stmt.bind_text(1, request_id.value);
    if (stmt.step_result() != SQLITE_DONE) return errmsg();

    return Result<bool>::Ok(true);
  }

  // ── find ────────────────────────────────────────────────────────────────────

  Result<std::optional<SavedAssignment>> find(
      const NodeId& node_id,
      const ModelId& model_id,
      const std::string& manifest_revision) const override {
    std::lock_guard lock(mutex_);
    if (!db_) return Result<std::optional<SavedAssignment>>::Ok(std::nullopt);

    Statement stmt(
        db_,
        "SELECT s.node_id, p.model_id, s.shard_id, p.manifest_revision, "
        "s.quantization_kind, s.backend_kind "
        "FROM stages s JOIN plans p ON s.plan_id = p.plan_id "
        "WHERE s.node_id = ? AND p.model_id = ? AND p.manifest_revision = ? "
        "ORDER BY p.fence_term DESC LIMIT 1");
    if (!stmt.valid())
      return Result<std::optional<SavedAssignment>>::Err(ErrorCode::kInternal,
                                                          sqlite3_errmsg(db_));
    stmt.bind_text(1, node_id.value);
    stmt.bind_text(2, model_id.value);
    stmt.bind_text(3, manifest_revision);

    if (!stmt.step())
      return Result<std::optional<SavedAssignment>>::Ok(std::nullopt);

    SavedAssignment a;
    a.node_id.value = std::string(stmt.column_text(0));
    a.model_id.value = std::string(stmt.column_text(1));
    a.shard_id.value = std::string(stmt.column_text(2));
    a.manifest_revision = std::string(stmt.column_text(3));
    a.quantization = static_cast<QuantizationKind>(stmt.column_int(4));
    a.backend = static_cast<BackendKind>(stmt.column_int(5));

    return Result<std::optional<SavedAssignment>>::Ok(std::move(a));
  }

 private:
  // ── Internal helpers ────────────────────────────────────────────────────────

  void create_tables() {
    const char* schema = R"SQL(
      CREATE TABLE IF NOT EXISTS plans (
        plan_id TEXT PRIMARY KEY,
        model_id TEXT NOT NULL,
        manifest_revision TEXT NOT NULL,
        fence_term INTEGER NOT NULL,
        fence_token TEXT NOT NULL,
        fence_membership_revision INTEGER NOT NULL,
        issued_at_epoch_ms INTEGER NOT NULL,
        valid_for_ms INTEGER NOT NULL,
        plan_json TEXT NOT NULL
      );
      CREATE TABLE IF NOT EXISTS stages (
        stage_id INTEGER PRIMARY KEY AUTOINCREMENT,
        plan_id TEXT NOT NULL REFERENCES plans(plan_id),
        model_id TEXT NOT NULL,
        ordinal INTEGER NOT NULL,
        node_id TEXT NOT NULL,
        shard_id TEXT NOT NULL,
        backend_kind INTEGER NOT NULL,
        quantization_kind INTEGER NOT NULL,
        reserved_memory_bytes INTEGER NOT NULL,
        estimated_stage_microseconds REAL NOT NULL
      );
      CREATE TABLE IF NOT EXISTS tokens (
        request_id TEXT NOT NULL,
        session_id TEXT NOT NULL,
        position INTEGER NOT NULL,
        token_id INTEGER NOT NULL,
        PRIMARY KEY (request_id, position)
      );
      CREATE INDEX IF NOT EXISTS idx_plans_model
        ON plans(model_id, manifest_revision);
      CREATE INDEX IF NOT EXISTS idx_stages_node
        ON stages(node_id, model_id);
    )SQL";

    char* err = nullptr;
    int rc = sqlite3_exec(db_, schema, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
      // Logging is not available in this translation unit; silently consume.
    }
    sqlite3_free(err);
  }

  bool exec(const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    sqlite3_free(err);
    return rc == SQLITE_OK;
  }

  void rollback() { exec("ROLLBACK"); }

  Result<bool> errmsg(const char* prefix = nullptr) {
    const char* msg = sqlite3_errmsg(db_);
    std::string full;
    if (prefix) {
      full += prefix;
      full += ": ";
    }
    full += msg ? msg : "unknown SQLite error";
    return Result<bool>::Err(ErrorCode::kInternal, std::move(full));
  }

  // ── Members ─────────────────────────────────────────────────────────────────

  std::string db_path_;
  sqlite3* db_{nullptr};
  mutable std::mutex mutex_;
};

// ── Factory function ──────────────────────────────────────────────────────────

std::unique_ptr<AssignmentStore> make_sqlite_assignment_store(
    const std::string& db_path) {
  return std::make_unique<SqliteAssignmentStore>(db_path);
}

}  // namespace socrates::persistence
