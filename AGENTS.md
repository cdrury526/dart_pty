# qdexcode MCP Tools

This project is indexed by **qdexcode**. Use the MCP tools below for code navigation, search, and exploration. These replace raw grep/find with richer results — symbol definitions, call graphs, impact analysis, and semantic search.

## Gateways

All commands go through gateway tools. Use `command` to select the operation and `args` for parameters.

| Gateway | Purpose |
|---|---|
| `code` | Symbol lookup, call graph navigation, module exploration |
| `search` | Full-text search, regex search, semantic search, file listing |
| `project` | Project stats, module breakdown, recent changes |

## Getting Oriented

Start every session by understanding the project:

```
project(command: "overview")                                    → scale, modules, key types, hot spots
code(command: "module_context", args: {module_path: "src"})     → drill into a directory
project(command: "tasks")                                       → discover build/test/lint targets
```

## Code Gateway

### extract — Deep-dive a symbol

Definition, source code, callers, and callees in one call. **Use this before modifying any function.**

```
code(command: "extract", args: {symbol: "functionName"})
code(command: "extract", args: {symbol: "functionName", include_source: true})
```

### module_context — Explore a directory

File count, symbols, language stats, inbound callers, outbound dependencies.

```
code(command: "module_context", args: {module_path: "src/components"})
code(command: "module_context", args: {module_path: "internal/api", limit: 100})
```

### symbol — Exact symbol lookup

Find a symbol by exact name. Returns file path, line number, kind, and signature.

```
code(command: "symbol", args: {name: "MyComponent"})
```

### resolve — Fuzzy symbol search

Partial or fuzzy name matching when you don't know the exact name.

```
code(command: "resolve", args: {name: "MyComp", limit: 10})
```

### callers — Who calls this?

All incoming call sites for a symbol.

```
code(command: "callers", args: {symbol: "fetchData"})
```

### callees — What does this call?

All outgoing calls from a symbol.

```
code(command: "callees", args: {symbol: "fetchData"})
```

### what_breaks_if — Impact analysis

Before changing a symbol's signature or deleting it, check what breaks.

```
code(command: "what_breaks_if", args: {symbol: "fetchData", change_type: "change-signature"})
code(command: "what_breaks_if", args: {symbol: "fetchData", change_type: "delete", max_depth: 3})
```

`change_type`: `"delete"` | `"change-signature"` | `"change-behavior"`

### graph_path — Trace call path between two symbols

```
code(command: "graph_path", args: {from: "handleRequest", to: "saveToDatabase", max_depth: 5})
```

## Search Gateway

### search — Unified search

Combines symbol name matching with semantic vector similarity.

```
search(command: "search", args: {query: "authentication"})
search(command: "search", args: {query: "authentication", include_source: true, limit: 10})
```

### content — Full-text search

Case-insensitive substring match across all indexed file contents.

```
search(command: "content", args: {query: "TODO", limit: 50})
```

### grep — Regex search

POSIX regex with optional glob filtering.

```
search(command: "grep", args: {pattern: "func.*New", glob: "*.go"})
search(command: "grep", args: {pattern: "import.*react", glob: "*.tsx"})
```

### files — List indexed files

Filter by language and/or path pattern.

```
search(command: "files", args: {language: "TypeScript", pattern: "src/**"})
search(command: "files", args: {language: "Go", limit: 200})
```

### context — RAG retrieval

Semantic search that returns symbol info, source code, callers, callees, and relevance scores. Best for natural-language questions about the codebase.

```
search(command: "context", args: {query: "how does authentication work", limit: 5})
```

## Project Gateway

### overview — Project stats

Scale, modules, key types, entry points, hot spots. Use `sections` to pick what you need.

```
project(command: "overview")
project(command: "overview", args: {sections: ["modules", "key_types"]})
project(command: "overview", args: {sections: ["all"]})
```

Sections: `scale`, `modules`, `key_types`, `hot_spots`, `entry_points`

### summary / snapshot — Quick stats

```
project(command: "summary")    → lightweight overview
project(command: "snapshot")   → raw stats only
```

### changes — Recently modified files

```
project(command: "changes", args: {since: "24h"})
project(command: "changes", args: {since: "7d"})
```

### tasks — Build/test/lint targets

Discovers Makefile targets, package.json scripts, CI configs.

```
project(command: "tasks")
```

## When to Use What

| I need to... | Command |
|---|---|
| Understand a function before editing | `code → extract` |
| Explore a directory I'm unfamiliar with | `code → module_context` |
| Find where something is called | `code → callers` |
| Check what breaks if I change something | `code → what_breaks_if` |
| Trace how A connects to B | `code → graph_path` |
| Search for a pattern in code | `search → grep` |
| Find files by language or path | `search → files` |
| Ask a natural-language question about code | `search → context` |
| Get project overview and structure | `project → overview` |
| Find build/test commands | `project → tasks` |