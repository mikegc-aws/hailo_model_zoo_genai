# Tool/Function Calling Support

## Overview

This document describes the implementation of tool/function calling support in the Hailo-Ollama server to achieve compatibility with the official Ollama API specification.

## Background

The Hailo-Ollama server provides an Ollama-compatible API for running LLM models on Hailo hardware. To support agentic workflows and function calling capabilities (as used by frameworks like Strands), the server needed to implement the Ollama tool calling specification.

## Changes Implemented

### 1. Data Transfer Objects (DTOs)

**File**: `src/library/dto/DTOs.hpp`

Added three new DTOs to support tool calling:

#### ToolCallFunction
```cpp
class ToolCallFunction: public oatpp::DTO {
    DTO_FIELD(Int32, index);      // Function index (0, 1, 2, ...)
    DTO_FIELD(String, name);       // Function name
    DTO_FIELD(Any, arguments);     // Function arguments as JSON object
};
```

#### ToolCall
```cpp
class ToolCall: public oatpp::DTO {
    DTO_FIELD(String, id);                        // Unique ID (e.g., "call_0")
    DTO_FIELD(String, type) = "function";         // Always "function"
    DTO_FIELD(Object<ToolCallFunction>, function); // Function details
};
```

#### ChatMessage (Enhanced)
```cpp
class ChatMessage: public oatpp::DTO {
    DTO_FIELD(String, role);
    DTO_FIELD(String, content);
    DTO_FIELD(Vector<Object<ToolCall>>, tool_calls);  // NEW
    DTO_FIELD(String, name);                          // NEW: for tool result messages
};
```

**Key Design Decision**: `arguments` is defined as `Any` type (not `String`) so it serializes as a JSON object instead of a JSON string. This matches the official Ollama API specification.

### 2. Request Processing

**File**: `src/library/controller/controller.cpp`

#### Tools Parameter Handling

The `/api/chat` endpoint now:
- Accepts the `tools` parameter in requests
- Transforms tools from Ollama format to template format
- Passes tools to the chat template for prompt generation

**Ollama Format** (received):
```json
{
  "type": "function",
  "function": {
    "name": "current_time",
    "description": "Return the current system time",
    "parameters": {"type": "object", "properties": {}}
  }
}
```

**Template Format** (passed to chat template):
```json
{
  "name": "current_time",
  "description": "Return the current system time",
  "parameters": {"type": "object", "properties": {}}
}
```

#### Message Normalization

To ensure compatibility with various clients (including stock Strands agents), the server automatically normalizes incoming messages:

1. **Content field normalization**: If a message has `tool_calls` but no `content` field, adds `content: ""`
2. **Tool call ID normalization**: If `tool_calls` are missing the `id` field, generates unique IDs
3. **Function index normalization**: If `function` objects are missing the `index` field, generates sequential indices

This allows the server to work with clients that don't preserve all required fields when sending tool calling messages.

### 3. Tool Call Detection and Parsing

**File**: `src/library/controller/controller.cpp`

Added `parse_tool_calls()` method that detects tool calls in model output using two strategies:

#### Strategy 1: Tagged Format
Detects `<tool_call>...</tool_call>` XML tags in the model output:
```
<tool_call>
{"name": "current_time", "arguments": {}}
</tool_call>
```

#### Strategy 2: Inline JSON Detection
Fallback parser that detects inline JSON function calls without tags:
```
{"name": "current_time", "arguments": {}}
```

The parser:
- Extracts tool calls from model output
- Removes tool call markup from content
- Converts JSON to proper `ToolCall` DTOs with object-typed arguments
- Generates missing `id` and `index` fields for compatibility

### 4. Response Format

**Non-streaming responses** (`stream: false`):

When tool calls are detected:
```json
{
  "model": "qwen2.5:1.5b",
  "created_at": "2026-01-21T15:00:00Z",
  "message": {
    "role": "assistant",
    "content": "",
    "tool_calls": [
      {
        "id": "call_0",
        "type": "function",
        "function": {
          "index": 0,
          "name": "current_time",
          "arguments": {}
        }
      }
    ]
  },
  "done": true,
  "done_reason": "tool_calls"
}
```

**Streaming responses** (`stream: true`):

The streaming handler:
- Detects when tool calls are being generated
- Buffers content instead of streaming it when `<tool_call>` is detected
- Sends the final chunk with `tool_calls` array and empty `content`

### 5. Streaming Optimizations

**File**: `src/library/controller/llm_generation_callback.cpp`

Added tool call detection during streaming:
- Detects `<tool_call>` opening tag as tokens arrive
- Stops sending intermediate content chunks when tool call is detected
- Buffers until `</tool_call>` closing tag is found
- Sends final chunk with parsed `tool_calls` array

This prevents sending partial tool call content to the client.

## API Compliance

The implementation follows the official Ollama API specification:

### Request Format

```json
{
  "model": "qwen2.5:1.5b",
  "messages": [
    {"role": "user", "content": "What time is it?"}
  ],
  "stream": true,
  "tools": [
    {
      "type": "function",
      "function": {
        "name": "current_time",
        "description": "Return the current system time",
        "parameters": {"type": "object", "properties": {}}
      }
    }
  ]
}
```

### Response Format Requirements

✅ **Content field**: Empty string `""` when tool_calls are present (not `null`)  
✅ **Tool call ID**: Unique identifier string (e.g., `"call_0"`)  
✅ **Function index**: Integer index in tool_calls array  
✅ **Arguments**: JSON object (not JSON string)  
✅ **Done reason**: Set to `"tool_calls"` when tool calls are present  
✅ **Usage metrics**: Always integers (never `null`)

## Compatibility Features

### Client Compatibility

The server is lenient with incoming requests to maximize compatibility:

1. **Missing `id` fields**: Automatically generated as `"call_0"`, `"call_1"`, etc.
2. **Missing `index` fields**: Automatically generated based on array position
3. **Missing `content` fields**: Automatically added as `""` when tool_calls are present
4. **Multiple tool formats**: Supports both tagged (`<tool_call>`) and inline JSON formats

This ensures compatibility with various client libraries, including:
- Stock Strands agents (`strands.models.ollama.OllamaModel`)
- Other Ollama-compatible clients

### Model Compatibility

The implementation works with chat templates that:
- Accept a `tools` array in the template inputs
- Generate prompts instructing the model to use `<tool_call>` XML tags
- Support the Qwen 2.5 tool calling format

Tested with:
- Qwen 2.5 1.5B
- Qwen 2.5 Coder 1.5B

## Usage Example

### Basic Tool Call Flow

1. **Client sends request with tools**:
```bash
curl -X POST http://localhost:8000/api/chat \
  -H "Content-Type: application/json" \
  -d '{
    "model": "qwen2.5:1.5b",
    "messages": [
      {"role": "user", "content": "What time is it?"}
    ],
    "stream": false,
    "tools": [
      {
        "type": "function",
        "function": {
          "name": "current_time",
          "description": "Return the current system time in ISO 8601 format",
          "parameters": {"type": "object", "properties": {}}
        }
      }
    ]
  }'
```

2. **Server returns tool call**:
```json
{
  "message": {
    "role": "assistant",
    "content": "",
    "tool_calls": [
      {
        "id": "call_0",
        "type": "function",
        "function": {
          "index": 0,
          "name": "current_time",
          "arguments": {}
        }
      }
    ]
  },
  "done": true,
  "done_reason": "tool_calls"
}
```

3. **Client executes tool and sends result back**:
```bash
curl -X POST http://localhost:8000/api/chat \
  -H "Content-Type: application/json" \
  -d '{
    "model": "qwen2.5:1.5b",
    "messages": [
      {"role": "user", "content": "What time is it?"},
      {
        "role": "assistant",
        "content": "",
        "tool_calls": [
          {
            "id": "call_0",
            "type": "function",
            "function": {
              "index": 0,
              "name": "current_time",
              "arguments": {}
            }
          }
        ]
      },
      {
        "role": "tool",
        "content": "2026-01-21T15:00:00",
        "name": "current_time"
      }
    ],
    "stream": false,
    "tools": [...]
  }'
```

4. **Server returns final response**:
```json
{
  "message": {
    "role": "assistant",
    "content": "The current time is 2026-01-21T15:00:00."
  },
  "done": true,
  "done_reason": "stop"
}
```

## Implementation Details

### Tool Call Parsing Algorithm

The `parse_tool_calls()` function:

1. Scans for `<tool_call>...</tool_call>` XML tags in the response
2. Extracts JSON content between tags
3. Parses JSON and creates `ToolCall` DTOs
4. Removes tool call markup from content string
5. Falls back to inline JSON detection if no tags found
6. Uses balanced brace matching to extract JSON objects
7. Generates missing `id` and `index` fields as needed

### Arguments Serialization

To ensure `arguments` serializes as a JSON object (not a string), the implementation:

1. Uses `oatpp::Any` type for the `arguments` field
2. Converts `nlohmann::json` to `oatpp::Any` recursively:
   - Objects → `oatpp::Fields<oatpp::Any>`
   - Arrays → `oatpp::Vector<oatpp::Any>`
   - Primitives → appropriate oatpp types
3. This ensures proper JSON serialization as objects instead of strings

### Error Handling

Enhanced error handling includes:
- Detailed JSON parsing error messages with position information
- Logging of problematic JSON regions
- Graceful handling of malformed tool calls
- Auto-generation of missing required fields

## Testing

### Verification

To verify tool calling is working correctly:

1. **Check tool detection**:
   - Server logs show: `"Transformed to X tools for template"`
   - Prompt includes tools in `<tools>...</tools>` section

2. **Check tool calls in response**:
   - Response includes `tool_calls` array when model decides to use tools
   - `content` is empty string when `tool_calls` are present
   - `done_reason` is `"tool_calls"`

3. **Check tool result handling**:
   - Server accepts messages with `role: "tool"`
   - Tool result messages include `name` field
   - Conversation continues correctly after tool execution

### Logs

Tool-related log messages to monitor:
```
chat:Tools string from DTO: [...]
chat:Successfully parsed X tools
chat:Transformed to X tools for template
chat:First transformed tool: {...}
chat:Tools section in prompt: <tools>...</tools>
chat:Added missing id to tool_call: call_X
chat:Added missing index to tool_call function: X
chat:Added missing content field (empty string) to message with tool_calls
```

## Compatibility Notes

### Official Ollama Compatibility

The implementation matches the official Ollama server behavior:
- Tool calls format matches exactly
- Request/response format is compatible
- Field types and structure align with the specification

### Client Compatibility

Tested and compatible with:
- Stock Strands agents (`strands-agents[ollama]>=1.22.0`)
- Standard Ollama clients

### Model Requirements

For tool calling to work, the model must:
- Be trained or fine-tuned for tool calling
- Understand the chat template's tool calling instructions
- Generate tool calls in the expected format (preferably with `<tool_call>` tags)

## Limitations

1. **No streaming of partial tool calls**: Tool calls are only sent in the final chunk
2. **Tool call detection**: Relies on model generating proper format (XML tags or JSON)
3. **Single response parsing**: Tool calls are parsed after generation completes
4. **Template dependency**: Requires chat templates with tool calling support

## Future Improvements

Potential enhancements:
- Streaming partial tool calls as they're generated
- Support for parallel tool calls (multiple tools in one response)
- Tool call validation against provided tool schemas
- Automatic retry on malformed tool calls
- Support for more tool calling formats

## References

- [Ollama API Documentation](https://github.com/ollama/ollama/blob/main/docs/api.md)
- [Strands Agent Framework](https://github.com/BismuthCloud/strands)
- [Qwen 2.5 Tool Calling](https://qwenlm.github.io/blog/qwen2.5/)

## Version History

- **v5.2.0**: Initial tool/function calling support
  - Added tool_calls support to `/api/chat` endpoint
  - Implemented tool call parsing and response formatting
  - Added message normalization for client compatibility
  - Enhanced streaming to handle tool calls
  - Full Ollama API compliance for tool calling
