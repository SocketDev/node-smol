// node:smol-manifest — uv.lock parser implementation (section walker).
//
// =====================================================================
// Source material (in lock-step order, newest → oldest)
// =====================================================================
//
// 1. **socket-sdxgen TS parser** — the public contract this native
//    impl matches (parseUvLock + normalizePep503):
//      socket-sdxgen/src/parsers/pypi/index.mts
//
// 2. **uv's canonical lockfile deserializer** — the structural model:
//    uv writes lockfiles in a predictable canonical format, so the
//    reader walks that layout directly instead of building a TOML
//    tree. Introduced by https://github.com/astral-sh/uv/pull/20648.
//    Pinned tag 0.12.1 (same pin as the .gitmodules upstream/uv
//    block and the xport.json row):
//      https://github.com/astral-sh/uv/blob/0.12.1/crates/uv-resolver/src/lock/deserialize.rs
//    The recognized-table registry this walker mirrors is
//    deserialize.rs:461-516 (section_key's header dispatch); the
//    "unknown or noncanonical lock table" rejection at root level is
//    deserialize.rs:508-512.
//
// 3. **uv's lockfile writer** — source of truth for what canonical
//    output contains:
//      https://github.com/astral-sh/uv/blob/0.12.1/crates/uv-resolver/src/lock/mod.rs
//
// Lock-step with the lexical half: parser_uv_scan.cc (trimming,
// key/value splitting, bracket depth, inline-table probing, PEP 503
// normalization).
//
// Lock-step note: uv's canonical reader falls back to its
// general-purpose TOML parser when it sees unsupported syntax
// (deserialize.rs:1-5). This port deliberately has NO such fallback —
// per repo doctrine a non-canonical uv.lock fails loud with
// ERR_INVALID_LOCKFILE naming the first unrecognized construct,
// because a silent fallback here would mean vendoring a second full
// TOML parser for input uv itself never writes.
//
// Lock-step note: comment lines (`# ...`) are skipped even though
// uv's canonical writer never emits them. Cargo's walker in this
// package does the same; tolerating comments costs one branch and
// keeps hand-annotated fixture inputs usable.
//
// =====================================================================
// Fix register
// =====================================================================
//
//   (none yet — first port)
//
// =====================================================================
// uv.lock notes (canonical layout, uv 0.12.1)
// =====================================================================
//
// - Top-level scalars: `version = 1`, `revision = N`,
//   `requires-python = ">=3.10"`, plus multi-line string arrays
//   (`resolution-markers`, `supported-markers`, `conflicts`,
//   `required-environments`). `version` drives lockVersion.
//
// - `[options]` (scalars incl. `exclude-newer`) and
//   `[options.exclude-newer-package]` (per-package date scalars) are
//   recognized and skimmed — no PackageRef output.
//
// - `[manifest]` carries inline-table arrays (`overrides = [` /
//   `constraints = [` ...), `[manifest.dependency-groups]`, and
//   `[[manifest.dependency-metadata]]`. Recognized and skimmed.
//
// - `[[package]]` repeats per resolved distribution. Field set:
//     name = "aiohttp"
//     version = "3.14.1"
//     source = { registry = "https://pypi.org/simple" }
//       (kinds: registry | git | url | path | directory | editable |
//        virtual — deserialize.rs's Source enum; git values carry
//        `<url>#<commit>` and map to vcsUrl/vcsCommit like the cargo
//        walker's `git+<url>#<commit>` split)
//     dependencies = [ { name = "aiosignal" }, ... ]
//       (inline tables, possibly with version/source/marker/extra
//        keys; entries may also be bare strings)
//     sdist = { url = "...", hash = "sha256:...", size = N, ... }
//       (hash → PackageRef.integrity, "sha256:" prefix kept verbatim)
//     wheels = [ { url = "...", hash = "...", size = N }, ... ]
//       (skimmed)
//
// - `[package.optional-dependencies]` / `[package.dev-dependencies]`
//   group tables: `group = [ { name = "h2" }, ... ]`. Names referenced
//   by an optional group mark that package row isOptional; dev groups
//   mark isDev (sdxgen's devNames pass). depType: dev > optional > prod.
//
// - `[package.metadata]` (+ `.requires-dev` / `.requires-dist`) is
//   recognized and skimmed.

#include "parser_uv.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "manifest.h"
#include "parser_uv_scan.h"

namespace node {
namespace socketsecurity {
namespace manifest {

namespace {

void AddToIndex(ParsedLockfile* out, std::string_view name, uint32_t idx) {
  for (auto& [k, v] : out->index) {
    if (k == name) {
      if (std::holds_alternative<uint32_t>(v)) {
        std::vector<uint32_t> vec{std::get<uint32_t>(v), idx};
        v = std::move(vec);
      } else {
        std::get<std::vector<uint32_t>>(v).push_back(idx);
      }
      return;
    }
  }
  out->index.emplace_back(name, PackageIndexValue{idx});
}

// Which construct the walker is inside. Mirrors deserialize.rs's
// MapKind (deserialize.rs:338-351) flattened to what this port needs.
enum class Section : uint8_t {
  kTop,
  kOptions,
  kManifest,
  kPackage,
  kGroups,
  kMetadata,
};

// What an open multi-line array's continuation lines mean.
enum class ArrayRole : uint8_t {
  kIgnore,
  kDeps,
  kGroup,
};

// Per-entry state for one [[package]] block.
struct UvEntry {
  std::string_view name;
  std::string_view version;
  std::string_view resolved;
  std::string_view integrity;
  std::string_view vcsUrl;
  std::string_view vcsCommit;
  std::vector<std::string_view> dependencies;
};

void FlushEntry(ParsedLockfile* out, UvEntry& e) {
  if (e.name.empty()) return;
  PackageRef ref;
  ref.name = e.name;
  ref.version = e.version;
  ref.resolved = e.resolved;
  ref.integrity = e.integrity;
  ref.vcsUrl = e.vcsUrl;
  ref.vcsCommit = e.vcsCommit;
  ref.dependencies = std::move(e.dependencies);
  ref.depType = DepType::kProd;
  out->packages.push_back(std::move(ref));
  AddToIndex(out, out->packages.back().name,
             static_cast<uint32_t>(out->packages.size() - 1));
}

// Apply the source inline table to the entry. Kind probing order
// matches deserialize.rs's Source enum arms; the first recognized
// kind key wins. registry / url kinds resolve to a fetchable URL →
// PackageRef.resolved; git carries `<url>#<commit>` → resolved +
// vcsUrl/vcsCommit (the cargo walker's split); local kinds (path /
// directory / editable / virtual) have no resolution URL, so
// resolved stays empty.
void ApplySource(std::string_view table, ParseContext* ctx, UvEntry* e) {
  std::string_view value;
  if (uvscan::InlineTableString(table, "registry", &value) ||
      uvscan::InlineTableString(table, "url", &value)) {
    e->resolved = ctx->intern.Intern(value);
    return;
  }
  if (uvscan::InlineTableString(table, "git", &value)) {
    e->resolved = ctx->intern.Intern(value);
    size_t hash = value.find('#');
    if (hash != std::string_view::npos) {
      e->vcsUrl = ctx->intern.Intern(value.substr(0, hash));
      e->vcsCommit = ctx->intern.Intern(value.substr(hash + 1));
    } else {
      e->vcsUrl = e->resolved;
    }
  }
}

bool FailUnrecognized(ParseError* err, size_t line_no,
                      std::string_view saw, const char* wanted) {
  std::string msg =
      "uv.lock is not in uv's canonical lockfile layout. Where: line ";
  msg += std::to_string(line_no);
  msg += ". Saw: `";
  msg.append(saw.substr(0, 120));
  msg += "`; wanted: ";
  msg += wanted;
  msg +=
      ". Fix: regenerate with `uv lock` — this parser reads only the "
      "canonical layout uv writes (astral-sh/uv#20648, pinned 0.12.1) "
      "and has no general-TOML fallback.";
  err->message = std::move(msg);
  err->code = "ERR_INVALID_LOCKFILE";
  return false;
}

// Top-level keys of the Lock wire struct (deserialize.rs's Root map
// plus the array-valued markers). Anything else is non-canonical.
bool IsKnownTopLevelKey(std::string_view key) {
  return key == "version" || key == "revision" ||
         key == "requires-python" || key == "resolution-markers" ||
         key == "supported-markers" || key == "required-environments" ||
         key == "conflicts";
}

}  // namespace

bool ParseUvLock(std::string_view content,
                 ParseContext* ctx,
                 ParsedLockfile* out,
                 ParseError* err) {
  out->ecosystem = Ecosystem::kPypi;
  // `version = N` at the top overrides below; every canonical
  // uv.lock carries it (deserialize.rs requires it), so this
  // pre-fill only shows for empty input.
  out->lockVersion = std::string_view{"1"};

  UvEntry cur;
  bool has_cur = false;
  Section section = Section::kTop;
  bool group_is_dev = false;

  // Open multi-line array state.
  bool in_array = false;
  ArrayRole array_role = ArrayRole::kIgnore;
  int array_depth = 0;

  // Names referenced by dev / optional group tables; a post-pass
  // flips flags on matching package rows (sdxgen's devNames pass).
  std::unordered_set<std::string_view, Fnv1aHash> dev_names;
  std::unordered_set<std::string_view, Fnv1aHash> optional_names;

  size_t pos = 0;
  size_t line_no = 0;
  while (pos < content.size()) {
    size_t eol = uvscan::NextLf(content, pos);
    std::string_view line = content.substr(pos, eol - pos);
    pos = eol + 1;
    ++line_no;

    std::string_view trimmed = uvscan::TrimAscii(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    // ---- Multi-line array continuation ----
    if (in_array) {
      array_depth += uvscan::NetBrackets(trimmed);
      if (array_depth <= 0) {
        in_array = false;
        continue;
      }
      if (array_role == ArrayRole::kIgnore) {
        continue;
      }
      if (trimmed[0] != '{' && trimmed[0] != '"') {
        return FailUnrecognized(
            err, line_no, trimmed,
            "a `{ name = ... }` inline table or quoted string entry");
      }
      std::string_view name = uvscan::ExtractUvDepName(trimmed, ctx);
      if (name.empty()) {
        continue;
      }
      if (array_role == ArrayRole::kDeps) {
        cur.dependencies.push_back(name);
      } else if (group_is_dev) {
        dev_names.insert(name);
      } else {
        optional_names.insert(name);
      }
      continue;
    }

    // ---- Section header ----
    //
    // Exact-match dispatch mirroring deserialize.rs:461-516. Any
    // header flushes the open [[package]] entry (all row fields
    // precede the sub-tables in canonical order); only [[package]]
    // opens a new one. An unknown header is the "unknown or
    // noncanonical lock table" rejection (deserialize.rs:508-512).
    if (trimmed[0] == '[') {
      if (has_cur) {
        FlushEntry(out, cur);
        cur = UvEntry{};
        has_cur = false;
      }
      if (trimmed == "[[package]]") {
        section = Section::kPackage;
        has_cur = true;
      } else if (trimmed == "[package.optional-dependencies]") {
        section = Section::kGroups;
        group_is_dev = false;
      } else if (trimmed == "[package.dev-dependencies]") {
        section = Section::kGroups;
        group_is_dev = true;
      } else if (trimmed == "[package.metadata]" ||
                 trimmed == "[package.metadata.requires-dev]" ||
                 trimmed == "[package.metadata.requires-dist]") {
        section = Section::kMetadata;
      } else if (trimmed == "[options]" ||
                 trimmed == "[options.exclude-newer-package]") {
        section = Section::kOptions;
      } else if (trimmed == "[manifest]" ||
                 trimmed == "[manifest.dependency-groups]" ||
                 trimmed == "[[manifest.dependency-metadata]]") {
        section = Section::kManifest;
      } else {
        return FailUnrecognized(
            err, line_no, trimmed,
            "a canonical uv.lock table ([options], [manifest], "
            "[[package]], or one of their documented sub-tables)");
      }
      continue;
    }

    // ---- key = value line ----
    std::string_view key = uvscan::KeyOf(trimmed);
    if (key.empty()) {
      return FailUnrecognized(err, line_no, trimmed,
                              "a `key = value` assignment");
    }
    std::string_view value = uvscan::ValueAfterEquals(trimmed);
    const int net = uvscan::NetBrackets(trimmed);

    // Multi-line array opener (`key = [` with unbalanced brackets).
    if (net > 0) {
      in_array = true;
      array_depth = net;
      if (section == Section::kPackage && key == "dependencies") {
        array_role = ArrayRole::kDeps;
      } else if (section == Section::kGroups) {
        array_role = ArrayRole::kGroup;
      } else {
        array_role = ArrayRole::kIgnore;
      }
      continue;
    }

    switch (section) {
      case Section::kTop:
        if (!IsKnownTopLevelKey(key)) {
          return FailUnrecognized(
              err, line_no, trimmed,
              "a top-level key uv's lockfile writer emits (version, "
              "revision, requires-python, resolution-markers, ...)");
        }
        if (key == "version") {
          out->lockVersion = ctx->intern.Intern(uvscan::StripQuotes(value));
        }
        break;
      case Section::kPackage:
        if (key == "name") {
          cur.name = uvscan::NormalizePep503(uvscan::StripQuotes(value), ctx);
        } else if (key == "version") {
          cur.version = ctx->intern.Intern(uvscan::StripQuotes(value));
        } else if (key == "source") {
          ApplySource(value, ctx, &cur);
        } else if (key == "sdist") {
          std::string_view hash;
          if (uvscan::InlineTableString(value, "hash", &hash)) {
            cur.integrity = ctx->intern.Intern(hash);
          }
        } else if (key == "dependencies") {
          // Single-line form: `dependencies = [{ name = "x" }]`.
          uvscan::ParseInlineEntries(value, ctx, &cur.dependencies);
        }
        // Other keys (wheels single-line, resolution-markers, future
        // canonical additions) are skimmed: shape already validated
        // by the `key = value` + balanced-bracket checks above.
        break;
      case Section::kGroups: {
        // Single-line group form: `http2 = [{ name = "h2" }]`.
        std::vector<std::string_view> names;
        uvscan::ParseInlineEntries(value, ctx, &names);
        for (std::string_view n : names) {
          if (group_is_dev) {
            dev_names.insert(n);
          } else {
            optional_names.insert(n);
          }
        }
        break;
      }
      case Section::kOptions:
      case Section::kManifest:
      case Section::kMetadata:
        // Recognized tables whose contents don't feed PackageRef
        // rows. The `key = value` shape check above still applies.
        break;
    }
  }

  if (in_array) {
    return FailUnrecognized(err, line_no, "<end of file>",
                            "a `]` closing the open array");
  }
  if (has_cur) {
    FlushEntry(out, cur);
  }

  // Post-pass: group-table flags. dev wins over optional for depType,
  // matching the JS-side depType precedence (dev > optional > prod).
  for (PackageRef& ref : out->packages) {
    const bool is_dev = dev_names.count(ref.name) > 0;
    const bool is_optional = optional_names.count(ref.name) > 0;
    if (is_dev) {
      ref.isDev = true;
      ref.depType = DepType::kDev;
    }
    if (is_optional) {
      ref.isOptional = true;
      if (!is_dev) {
        ref.depType = DepType::kOptional;
      }
    }
  }

  return true;
}

}  // namespace manifest
}  // namespace socketsecurity
}  // namespace node
