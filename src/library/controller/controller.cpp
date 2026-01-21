/**
 * Copyright (c) 2019-2025 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
/**
 * @file controller.cpp
 * @brief MyController implementation
 **/

#include "controller.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#include <hailo/genai/llm/llm.hpp>
#include <minja/chat-template.hpp>
#include <oatpp/base/Log.hpp>
#include <oatpp/data/mapping/ObjectMapper.hpp>
#include <oatpp/macro/codegen.hpp>
#include <oatpp/macro/component.hpp>
#include <oatpp/web/protocol/http/outgoing/StreamingBody.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "config/static_config.hpp"
#include "controller/llm_generation_callback.hpp"
#include "controller/pull_callback.hpp"
#include "dto/DTOs.hpp"
#include "generation_context/generation_context.hpp"
#include "model/resource.hpp"
#include "model/store.hpp"
#include "oatpp/Types.hpp"
#include "utils/time.hpp"

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

// manage all long imports  from oatpp
namespace oat {
using OutgoingResponse = oatpp::web::protocol::http::outgoing::Response;
using OutgoingStreamingBody =
    oatpp::web::protocol::http::outgoing::StreamingBody;
}  // namespace oat

namespace {
void set_options(
    const oatpp::Object<ModelParameters>& options,
    Generation& generation
) {
    if (!options) {
        return;
    }
    if (options->temperature == 0.0F) {
        generation.do_sample = false;
    } else if (options->temperature != nullptr) {
        generation.do_sample = true;
        generation.temperature = options->temperature;
    }

    if (options->seed != nullptr && options->seed != -1) {
        generation.seed = options->seed;
    }

    if (options->top_k != nullptr) {
        generation.top_k = options->top_k;
    }
    if (options->top_p != nullptr) {
        generation.top_p = options->top_p;
    }
    if (options->frequency_penalty != nullptr) {
        generation.frequency_penalty = options->frequency_penalty;
    }

    if (options->num_predict != nullptr) {
        generation.max_generated_tokens = options->num_predict;
    }
}
}  // namespace

MyController::MyController(
    const std::shared_ptr<SyncGenerationContext>& generation_context,
    const std::shared_ptr<ModelStore>& model_store,
    const std::shared_ptr<ResourceProvider>& resource_provider,
    const std::shared_ptr<oatpp::web::mime::ContentMappers>& apiContentMappers
) :
    oatpp::web::server::api::ApiController(apiContentMappers),
    m_generation_context(generation_context),
    m_model_store(model_store),
    m_resource_provider(resource_provider) {}

std::optional<std::pair<ModelInfo, std::filesystem::path>>
MyController::get_model_data(const std::string& model_name) {
    const auto model_data_opt = m_model_store->get_model(model_name);
    if (!model_data_opt) {
        return std::nullopt;
    }
    const auto& model_data = *model_data_opt;
    const auto hef = m_resource_provider->get_resource(model_data.hef_resource);
    if (!fs::is_regular_file(hef)) {
        return std::nullopt;
    }

    return {{model_data, hef}};
}

std::optional<oatpp::Object<ModelInfoShort>>
MyController::get_model_info(const std::string& model_name) {
    const auto model_data_opt = get_model_data(model_name);
    if (!model_data_opt) {
        return std::nullopt;
    }
    const auto& model_data = model_data_opt->first;
    const auto& hef = model_data_opt->second;
    std::error_code error_code;
    const auto modified_at = fs::last_write_time(hef, error_code);
    if (error_code) {
        return std::nullopt;
    }
    const auto file_size = fs::file_size(hef, error_code);
    if (error_code) {
        return std::nullopt;
    }
    auto model_info = ModelInfoShort::createShared();
    model_info->name = model_name;
    model_info->model = model_name;
    model_info->size = file_size;
    model_info->modified_at = to_iso_8601(modified_at, "Z");
    if (!model_data.details.empty()) {
        model_info->details =
            m_contentMappers->getDefaultMapper()
                ->readFromString<oatpp::Object<ModelInfoDetails>>(
                    model_data.details
                );
    }
    return model_info;
}

std::optional<std::chrono::seconds>
MyController::convert_keep_alive(const oatpp::Int32& keep_alive) {
    if (!keep_alive) {
        return config::
            controller_default_keep_alive;  // default value for keep_alive is 5m
    } else if (*keep_alive < 0) {
        return std::nullopt;
    } else {
        return std::chrono::seconds(*keep_alive);
    }
}

std::pair<std::string, oatpp::Vector<oatpp::Object<ToolCall>>>
MyController::parse_tool_calls(const std::string& response) {
    auto tool_calls = oatpp::Vector<oatpp::Object<ToolCall>>::createShared();
    std::string cleaned_content = response;

    // Helper to convert nlohmann::json to oatpp::Any so that arguments serialize as an object
    std::function<oatpp::Any(const json&)> json_to_any = [&](const json& j) -> oatpp::Any {
        if (j.is_object()) {
            auto map = oatpp::Fields<oatpp::Any>::createShared();
            for (auto it = j.cbegin(); it != j.cend(); ++it) {
                map[it.key()] = json_to_any(it.value());
            }
            return map;
        }
        if (j.is_array()) {
            auto vec = oatpp::Vector<oatpp::Any>::createShared();
            for (const auto& v : j) {
                vec->push_back(json_to_any(v));
            }
            return vec;
        }
        if (j.is_string()) {
            return oatpp::Any(oatpp::String(j.get<std::string>().c_str()));
        }
        if (j.is_boolean()) {
            return oatpp::Any(oatpp::Boolean(j.get<bool>()));
        }
        if (j.is_number_integer()) {
            return oatpp::Any(oatpp::Int64(j.get<long long>()));
        }
        if (j.is_number_unsigned()) {
            return oatpp::Any(oatpp::Int64(static_cast<long long>(j.get<unsigned long long>())));
        }
        if (j.is_number_float()) {
            return oatpp::Any(oatpp::Float64(j.get<double>()));
        }
        // null
        return oatpp::Any(nullptr);
    };
    
    // Look for <tool_call>...</tool_call> tags
    const std::string open_tag = "<tool_call>";
    const std::string close_tag = "</tool_call>";
    
    size_t pos = 0;
    size_t call_id = 0;
    
    while ((pos = cleaned_content.find(open_tag, pos)) != std::string::npos) {
        size_t tag_start = pos;
        size_t content_start = pos + open_tag.length();
        size_t tag_end = cleaned_content.find(close_tag, content_start);
        
        if (tag_end == std::string::npos) {
            // Malformed - no closing tag, skip
            break;
        }
        
        // Extract JSON content between tags
        std::string json_content = cleaned_content.substr(
            content_start,
            tag_end - content_start
        );
        
        // Trim whitespace
        json_content.erase(0, json_content.find_first_not_of(" \t\n\r"));
        json_content.erase(json_content.find_last_not_of(" \t\n\r") + 1);
        
        try {
            // Parse the JSON
            auto tool_call_json = json::parse(json_content);
            
            if (tool_call_json.contains("name") && tool_call_json.contains("arguments")) {
                auto tool_call = ToolCall::createShared();
                // Generate id if missing (for compatibility with clients that don't preserve it)
                if (tool_call_json.contains("id")) {
                    tool_call->id = tool_call_json["id"].get<std::string>();
                } else {
                    tool_call->id = "call_" + std::to_string(call_id++);
                }
                tool_call->type = "function";
                
                auto function = ToolCallFunction::createShared();
                // Generate index if missing (for compatibility with clients that don't preserve it)
                if (tool_call_json.contains("function") && tool_call_json["function"].is_object()) {
                    const auto& func_obj = tool_call_json["function"];
                    if (func_obj.contains("index")) {
                        function->index = static_cast<oatpp::Int32>(func_obj["index"].get<int>());
                    } else {
                        function->index = static_cast<oatpp::Int32>(tool_calls->size());
                    }
                    if (func_obj.contains("name")) {
                        function->name = func_obj["name"].get<std::string>();
                    } else {
                        function->name = tool_call_json["name"].get<std::string>();
                    }
                    if (func_obj.contains("arguments")) {
                        function->arguments = json_to_any(func_obj["arguments"]);
                    } else {
                        function->arguments = json_to_any(tool_call_json["arguments"]);
                    }
                } else {
                    // Legacy format: name and arguments at top level
                    function->index = static_cast<oatpp::Int32>(tool_calls->size());
                    function->name = tool_call_json["name"].get<std::string>();
                    function->arguments = json_to_any(tool_call_json["arguments"]);
                }
                
                tool_call->function = function;
                tool_calls->push_back(tool_call);
            }
        } catch (const json::parse_error& e) {
            OATPP_LOGw("parse_tool_calls", "Failed to parse tool call JSON: {}", e.what());
        }
        
        // Remove the tool_call tag from cleaned content
        cleaned_content.erase(tag_start, tag_end + close_tag.length() - tag_start);
        pos = tag_start;  // Check from this position again
    }

    // Fallback parsing: detect inline JSON function calls without <tool_call> tags
    // Example the model might emit:
    // {"function": "current_time", "arguments": {}}
    // or {"name": "current_time", "arguments": {}}
    auto try_parse_inline_tool = [&](const std::string& json_str) {
        try {
            auto obj = json::parse(json_str);
            std::string name;
            json arguments = json::object();

            if (obj.contains("function") && obj["function"].is_string()) {
                name = obj["function"].get<std::string>();
            } else if (obj.contains("name") && obj["name"].is_string()) {
                name = obj["name"].get<std::string>();
            }
            if (obj.contains("arguments")) {
                arguments = obj["arguments"];
            } else if (obj.contains("parameters")) {
                arguments = obj["parameters"];
            }
            if (!name.empty()) {
                auto tool_call = ToolCall::createShared();
                // Generate id if missing
                if (obj.contains("id")) {
                    tool_call->id = obj["id"].get<std::string>();
                } else {
                    tool_call->id = "call_" + std::to_string(tool_calls->size());
                }
                tool_call->type = "function";

                auto function = ToolCallFunction::createShared();
                // Generate index if missing
                if (obj.contains("function") && obj["function"].is_object()) {
                    const auto& func_obj = obj["function"];
                    if (func_obj.contains("index")) {
                        function->index = static_cast<oatpp::Int32>(func_obj["index"].get<int>());
                    } else {
                        function->index = static_cast<oatpp::Int32>(tool_calls->size());
                    }
                } else {
                    function->index = static_cast<oatpp::Int32>(tool_calls->size());
                }
                function->name = name;
                function->arguments = json_to_any(arguments);
                tool_call->function = function;
                tool_calls->push_back(tool_call);
                return true;
            }
        } catch (const std::exception& e) {
            // ignore
        }
        return false;
    };

    // Try to find JSON blocks in the cleaned_content by scanning for balanced braces
    auto find_balanced_json = [](const std::string& text, size_t start_pos) -> std::optional<std::pair<size_t, size_t>> {
        bool in_string = false;
        bool escape = false;
        int depth = 0;
        for (size_t i = start_pos; i < text.size(); ++i) {
            char c = text[i];
            if (escape) {
                escape = false;
                continue;
            }
            if (c == '\\') {
                escape = true;
                continue;
            }
            if (c == '\"') {
                in_string = !in_string;
                continue;
            }
            if (in_string) {
                continue;
            }
            if (c == '{') {
                if (depth == 0) {
                    start_pos = i;
                }
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) {
                    return std::make_pair(start_pos, i);
                }
            }
        }
        return std::nullopt;
    };

    size_t search_pos = 0;
    while (true) {
        auto brace_pos = cleaned_content.find('{', search_pos);
        if (brace_pos == std::string::npos) {
            break;
        }
        auto json_range = find_balanced_json(cleaned_content, brace_pos);
        if (!json_range) {
            break;
        }
        auto [start, end] = *json_range;
        std::string candidate = cleaned_content.substr(start, end - start + 1);
        if (try_parse_inline_tool(candidate)) {
            // Remove parsed JSON from content
            cleaned_content.erase(start, end - start + 1);
            search_pos = start;  // continue from here
        } else {
            search_pos = end + 1;
        }
    }
    
    // Clean up any remaining whitespace/newlines
    while (!cleaned_content.empty() && 
           (cleaned_content.back() == '\n' || cleaned_content.back() == ' ' || cleaned_content.back() == '\r')) {
        cleaned_content.pop_back();
    }
    
    return {cleaned_content, tool_calls};
}

std::shared_ptr<oat::OutgoingResponse> MyController::handle_completion(
    const ModelInfo& model_data,
    const std::string& raw_prompt,
    const oatpp::Object<ModelParameters>& options,
    const bool stream,
    const oatpp::Int32& keep_alive,
    const std::string& model,
    const ReturnType return_type
) {
    using GenerationStatus = hailort::genai::LLMGeneratorCompletion::Status;

    const auto hef = m_resource_provider->get_resource(model_data.hef_resource);
    OATPP_LOGi("handle_completion", "Got model {}", hef.string());
    Generation generation {
        .model_name = model_data.name,
        .model_path = hef,
        .prompt = raw_prompt,
        .top_p = model_data.generation_params.top_p,
        .top_k = model_data.generation_params.top_k,
        .frequency_penalty = model_data.generation_params.frequency_penalty,
    };
    generation.keep_alive = convert_keep_alive(keep_alive);

    if (model_data.generation_params.temperature) {
        if (*model_data.generation_params.temperature != 0.0F) {
            generation.temperature = *model_data.generation_params.temperature;
        } else {
            generation.do_sample = false;
        }
    }
    set_options(options, generation);
    auto generator = m_generation_context->lock();
    auto generator_completion = generator->generate_one(std::move(generation));
    const auto& stop_tokens = model_data.generation_params.stop_tokens;
    if (!stream) {
        std::stringstream response;

        auto token_count = 0ULL;
        const std::chrono::steady_clock::time_point begin =
            std::chrono::steady_clock::now();
        bool stop_token_encountered = false;
        while (true) {
            const auto status = generator_completion.generation_status();
            if (status == GenerationStatus::LOGICAL_END_OF_GENERATION) {
                stop_token_encountered = true;
                break;
            }
            if (status == GenerationStatus::MAX_TOKENS_REACHED) {
                break;
            }

            const auto output =
                generator_completion.read().expect("read failed!");

            // check status immediately after read to see if it's the last one
            const auto is_last_token =
                (generator_completion.generation_status()
                 != GenerationStatus::GENERATING);
            // the last token is guaranteed to be an end token -> not returning to user or to history
            if (is_last_token) {
                continue;
            }

            const auto stop_token_it =
                std::find(stop_tokens.cbegin(), stop_tokens.cend(), output);
            if (stop_token_it != stop_tokens.cend()) {
                stop_token_encountered = true;
                // Do not return stop tokens to user, but they're still part of the last prompt
                generator->append_last_prompt(output);
                break;
            }
            ++token_count;
            response << output;
        }

        // we'd like to stop the generation on stop_token but it leads to races
        // instead we continue reading until HRT stops the generation
        while (true) {
            const auto status = generator_completion.generation_status();
            if (status != GenerationStatus::GENERATING) {
                break;
            }
            const auto output =
                generator_completion.read().expect("read failed!");

            // check status immediately after read to see if it's the last one
            const auto is_last_token =
                (generator_completion.generation_status()
                 != GenerationStatus::GENERATING);
            // the last token is guaranteed to be an end token -> not returning to user or to history
            if (is_last_token) {
                break;
            }

            generator->append_last_prompt(output);
        }

        const std::chrono::steady_clock::time_point end =
            std::chrono::steady_clock::now();
        std::string stop_reason = stop_token_encountered ? "stop" : "length";

        // Parse tool calls from response
        auto [cleaned_content, tool_calls] = MyController::parse_tool_calls(response.str());
        bool has_tool_calls = tool_calls && tool_calls->size() > 0;

        if (return_type == ReturnType::COMPLETION) {
            auto result = CreateChatCompletionResponse::createShared();
            result->id = "chatcmpl-" + std::to_string(std::rand());
            result->object = "chat.completion";
            result->created =
                std::chrono::system_clock::now().time_since_epoch().count();

            result->model = model;
            result->choices = {};

            auto message = ChatCompletionMessage::createShared();
            message->role = "assistant";
            message->content = cleaned_content;

            auto choice = ChatChoice::createShared();
            choice->index = 0L;
            choice->finish_reason = has_tool_calls ? "tool_calls" : std::move(stop_reason);
            choice->message = message;

            result->choices->push_back(choice);
            return createDtoResponse(Status::CODE_200, result);
        }
        auto result = GenerationResponseFinal::createShared();
        result->model = model;
        result->created_at = get_current_time_formatted();
        if (return_type == ReturnType::MESSAGE) {
            result->message = ChatMessage::createShared();
            result->message->role = "assistant";
            // When tool_calls are present, content must be empty string per Ollama API spec
            if (has_tool_calls) {
                result->message->content = "";
                result->message->tool_calls = tool_calls;
            } else {
                result->message->content = cleaned_content;
            }
        } else {
            result->response = cleaned_content;
        }
        generator->append_last_prompt(response.str());

        result->done = true;
        result->done_reason = has_tool_calls ? "tool_calls" : std::move(stop_reason);
        const auto total_time_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                .count();
        result->total_duration = total_time_ns;
        result->eval_count = token_count;
        // Explicitly set prompt_eval_count to 0 to ensure it's never null
        result->prompt_eval_count = 0UL;
        // Set other optional metadata fields to 0 to ensure they're never null
        result->load_duration = 0UL;
        result->eval_duration = 0UL;
        result->prompt_eval_duration = 0UL;

        return createDtoResponse(Status::CODE_200, result);
    }
    auto body = std::make_shared<oat::OutgoingStreamingBody>(
        std::make_shared<LLMGenerationReadCallback>(
            model,
            stop_tokens,
            m_contentMappers->getDefaultMapper(),
            std::move(generator),
            std::move(generator_completion),
            return_type == ReturnType::MESSAGE
        )
    );

    auto outgoing_response =
        OutgoingResponse::createShared(Status::CODE_200, body);
    outgoing_response->putHeader("Content-Type", "application/x-ndjson");
    return outgoing_response;
}

std::shared_ptr<oat::OutgoingResponse> MyController::handle_load_unload(
    const std::string& model_name,
    const ModelInfo& model_data,
    const oatpp::Object<ModelParameters>& options,
    const oatpp::Int32& keep_alive,
    const bool return_as_message
) {
    (void)options;
    // keep alive is 0 -> should unload the model
    auto result = GenerationResponseFinal::createShared();
    auto generator = m_generation_context->lock();
    if (keep_alive && *keep_alive == 0) {
        generator->reset();

        result->done_reason = "unload";
    } else {
        // Model load
        const auto hef =
            m_resource_provider->get_resource(model_data.hef_resource);
        generator->load_model(model_name, hef, convert_keep_alive(keep_alive));
        result->done_reason = "load";
    }
    result->model = model_name;
    result->created_at = get_current_time_formatted();
    if (return_as_message) {
        result->message = ChatMessage::createShared();
        result->message->role = "assistant";
        result->message->content = "";
    } else {
        result->response = "";
    }
    result->done = true;
    return createDtoResponse(Status::CODE_200, result);
}

std::shared_ptr<oat::OutgoingResponse> MyController::root() {
    return ResponseFactory::createResponse(
        Status::CODE_200,
        "hailo-ollama is running"
    );
}

std::shared_ptr<oat::OutgoingResponse> MyController::version() {
    auto result = VersionResponse::createShared();
    // tools might rely on this -> return a version similar to original Ollama
    result->version = "0.5.1";
    return createDtoResponse(Status::CODE_200, result);
}

std::shared_ptr<oat::OutgoingResponse> MyController::list_models() {
    const auto model_names = m_model_store->get_model_names();
    OATPP_LOGi("list_models", "got {} models in store", model_names.size());
    auto result = TagsResponse::createShared();
    result->models = {};
    for (const auto& model_name : model_names) {
        OATPP_LOGi("list_models", "model: {}", model_name);
        const auto model_info = get_model_info(model_name);
        if (!model_info) {
            continue;
        }
        result->models->push_back(*model_info);
    }
    return createDtoResponse(Status::CODE_200, result);
}

std::shared_ptr<oat::OutgoingResponse> MyController::list_all_models() {
    const auto model_names = m_model_store->get_model_names();
    OATPP_LOGi("list_models", "got {} models in store", model_names.size());
    auto result = ListAllResponse::createShared();
    result->models = {};
    for (const auto& name : model_names) {
        result->models->push_back(name);
    }

    return createDtoResponse(Status::CODE_200, result);
}

std::shared_ptr<oat::OutgoingResponse> MyController::list_running_models() {
    auto generator = m_generation_context->lock();
    const auto model_name = generator->get_model_name();
    const auto expiration = generator->get_expiration();
    generator.reset();  // unlock
    auto result = TagsResponse::createShared();
    result->models = {};
    const auto model_info_opt = get_model_info(model_name);
    if (model_info_opt) {
        const auto& model_info = *model_info_opt;
        model_info->expires_at = to_iso_8601(expiration, "Z");
        result->models->push_back(model_info);
    }

    return createDtoResponse(Status::CODE_200, result);
}

std::shared_ptr<oat::OutgoingResponse>
MyController::show(const oatpp::Object<ShowParams>& show_params) {
    const auto model_data_opt = get_model_data(show_params->model);
    if (!model_data_opt) {
        auto error_result = ErrorResponse::createShared();
        error_result->error = "model '" + show_params->model + "' not found";
        return createDtoResponse(Status::CODE_200, error_result);
    }
    const auto& model_data = model_data_opt->first;
    auto result = ShowResponse::createShared();
    result->license = model_data.license;
    result->modelfile = "";

    std::stringstream parameters_stream;
    for (const auto& stop_token : model_data.generation_params.stop_tokens) {
        parameters_stream << "stop"
                          << std::setw(config::controller_show_parameter_width)
                          << '"' << stop_token << '"' << '\n';
    }
    std::string parameters_string = parameters_stream.str();
    // remove final \n
    if (!parameters_string.empty()) {
        parameters_string.pop_back();
    }
    result->parameters = std::move(parameters_string);
    result->chat_template = model_data.template_params.chat_template;
    if (!model_data.details.empty()) {
        result->details = m_contentMappers->getDefaultMapper()
                              ->readFromString<oatpp::Object<ModelInfoDetails>>(
                                  model_data.details
                              );
    }
    result->model_info = "";
    std::error_code error_code;
    const auto& hef = model_data_opt->second;
    const auto modified_at = fs::last_write_time(hef, error_code);
    if (!error_code) {
        result->modified_at = to_iso_8601(modified_at, "Z");
    }
    return createDtoResponse(Status::CODE_200, result);
}

std::shared_ptr<oat::OutgoingResponse>
MyController::pull_model(const oatpp::Object<PullParams>& pull_params) {
    auto model_data = m_model_store->get_model(pull_params->model);
    if (!model_data) {
        auto error_result = ErrorResponse::createShared();
        error_result->error = "model '" + pull_params->model + "' not found";
        return createDtoResponse(Status::CODE_200, error_result);
    }

    if (!pull_params->stream) {
        m_resource_provider->pull_resource(model_data->hef_resource);
        auto result = PullResponse::createShared();
        result->status = "success";

        return createDtoResponse(Status::CODE_200, result);
    }

    auto queue = std::make_shared<PullReadCallback::EventQueue>();
    std::thread pull_thread(
        [this, &model_data, queue, hef_resource = model_data->hef_resource]() {
            m_resource_provider->pull_resource(hef_resource, queue);
        }
    );
    auto body = std::make_shared<oat::OutgoingStreamingBody>(
        std::make_shared<PullReadCallback>(
            m_contentMappers->getDefaultMapper(),
            queue,
            std::move(pull_thread)
        )
    );

    auto outgoing_response =
        OutgoingResponse::createShared(Status::CODE_200, body);
    outgoing_response->putHeader("Content-Type", "application/x-ndjson");
    return outgoing_response;
}

std::shared_ptr<oat::OutgoingResponse>
MyController::delete_model(const oatpp::Object<DeleteParams>& delete_params) {
    auto model_data = m_model_store->get_model(delete_params->model);
    if (!model_data) {
        auto error_result = DeleteErrorResponse::createShared();
        error_result->code = "not_found";
        error_result->error = "model not found";
        return createDtoResponse(Status::CODE_404, error_result);
    }
    const auto hef =
        m_resource_provider->get_resource(model_data->hef_resource);
    std::error_code error_code;
    const auto removed = fs::remove(hef, error_code);
    if (error_code || !removed) {
        auto error_result = DeleteErrorResponse::createShared();
        error_result->code = "not_found";
        error_result->error = "model not found";
        return createDtoResponse(Status::CODE_404, error_result);
    }

    return createResponse(Status::CODE_200);
}

std::shared_ptr<oat::OutgoingResponse> MyController::generate(
    const oatpp::Object<GenerationParams>& generation_params
) {
    const auto& model = generation_params->model;

    const auto model_data_opt = get_model_data(model);
    if (!model_data_opt) {
        auto error_result = ErrorResponse::createShared();
        error_result->error = "model '" + model + "' not found";
        return createDtoResponse(Status::CODE_200, error_result);
    }
    const auto& model_data = model_data_opt->first;

    if (!generation_params->prompt) {
        return handle_load_unload(
            model,
            model_data,
            generation_params->options,
            generation_params->keep_alive,
            false
        );
    }
    minja::chat_template templ(
        model_data.template_params.chat_template,
        model_data.template_params.bos_token,
        model_data.template_params.eos_token
    );

    const auto& prompt = generation_params->prompt;
    const auto stream = generation_params->stream;

    minja::chat_template_inputs inputs;
    inputs.add_generation_prompt = true;
    inputs.messages = json {{{"role", "user"}, {"content", prompt}}};

    const std::string prompt_templ = templ.apply(inputs);

    return handle_completion(
        model_data,
        prompt_templ,
        generation_params->options,
        stream,
        generation_params->keep_alive,
        model,
        ReturnType::RESPONSE
    );
}

std::shared_ptr<oat::OutgoingResponse>
MyController::chat(const oatpp::Object<ChatParams>& generation_params) {
    const auto& model = generation_params->model;
    const auto model_data_opt = get_model_data(model);
    if (!model_data_opt) {
        auto error_result = ErrorResponse::createShared();
        error_result->error = "model '" + model + "' not found";
        return createDtoResponse(Status::CODE_200, error_result);
    }
    const auto& model_data = model_data_opt->first;

    if (!generation_params->messages) {
        return handle_load_unload(
            model,
            model_data,
            generation_params->options,
            generation_params->keep_alive,
            true
        );
    }
    minja::chat_template templ(
        model_data.template_params.chat_template,
        model_data.template_params.bos_token,
        model_data.template_params.eos_token
    );

    const auto stream = generation_params->stream;

    minja::chat_template_inputs inputs;
    inputs.add_generation_prompt = true;
    
    // Normalize tool_calls in DTO before serialization to avoid invalid JSON
    // Some clients (like Strands) don't preserve id and index fields
    if (generation_params->messages) {
        for (auto& message : *generation_params->messages) {
            if (message && message->tool_calls) {
                size_t tool_call_index = 0;
                for (auto& tool_call : *message->tool_calls) {
                    if (tool_call) {
                        // Generate id if missing
                        if (!tool_call->id || tool_call->id->empty()) {
                            tool_call->id = "call_" + std::to_string(tool_call_index);
                            OATPP_LOGi("chat", "Added missing id to tool_call: {}", tool_call->id->c_str());
                        }
                        // Generate function.index if missing
                        if (tool_call->function) {
                            if (!tool_call->function->index) {
                                tool_call->function->index = static_cast<oatpp::Int32>(tool_call_index);
                                OATPP_LOGi("chat", "Added missing index to tool_call function: {}", tool_call_index);
                            }
                        }
                    }
                    ++tool_call_index;
                }
            }
        }
    }
    
    // Parse messages with error handling and detailed logging
    try {
        const auto messages_str = m_contentMappers->getDefaultMapper()
                                    ->writeToString(generation_params->messages)
                                    .getValue("");
        OATPP_LOGi("chat", "Messages JSON string length: {}", messages_str.length());
        
        // Log a snippet of the JSON for debugging (first 500 chars and last 200 chars)
        if (messages_str.length() > 500) {
            OATPP_LOGi("chat", "Messages JSON start: {}", messages_str.substr(0, 500));
            OATPP_LOGi("chat", "Messages JSON end: {}", messages_str.substr(messages_str.length() - 200));
        } else {
            OATPP_LOGi("chat", "Messages JSON full: {}", messages_str);
        }
        
        // If error occurs around column 325, log that region
        if (messages_str.length() > 325) {
            size_t start = (325 > 100) ? 325 - 100 : 0;
            size_t len = std::min(static_cast<size_t>(200), messages_str.length() - start);
            OATPP_LOGi("chat", "Messages JSON around column 325: {}", messages_str.substr(start, len));
        }
        
        inputs.messages = json::parse(messages_str);
        OATPP_LOGi("chat", "Successfully parsed {} messages", inputs.messages.size());
        
        // Normalize messages: ensure content field exists when tool_calls are present
        // Per Ollama spec: content must be "" (empty string) when tool_calls are present
        for (auto& message : inputs.messages) {
            if (message.contains("tool_calls") && message["tool_calls"].is_array() && !message["tool_calls"].empty()) {
                // If tool_calls are present, ensure content field exists (even if empty)
                if (!message.contains("content") || message["content"].is_null()) {
                    message["content"] = "";
                    OATPP_LOGi("chat", "Added missing content field (empty string) to message with tool_calls");
                }
            }
        }
        
        // Normalize tool_calls: add missing id and index fields for compatibility
        // Some clients (like Strands) don't preserve these fields when sending tool_calls back
        for (auto& message : inputs.messages) {
            if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                size_t tool_call_index = 0;
                for (auto& tool_call : message["tool_calls"]) {
                    // Add id if missing
                    if (!tool_call.contains("id") || tool_call["id"].is_null()) {
                        tool_call["id"] = "call_" + std::to_string(tool_call_index);
                        OATPP_LOGi("chat", "Added missing id to tool_call: {}", tool_call["id"].dump());
                    }
                    // Add function.index if missing
                    if (tool_call.contains("function") && tool_call["function"].is_object()) {
                        auto& func = tool_call["function"];
                        if (!func.contains("index") || func["index"].is_null()) {
                            func["index"] = static_cast<int>(tool_call_index);
                            OATPP_LOGi("chat", "Added missing index to tool_call function: {}", tool_call_index);
                        }
                    }
                    ++tool_call_index;
                }
            }
        }
        
        // Log message roles for debugging
        for (size_t i = 0; i < inputs.messages.size(); ++i) {
            if (inputs.messages[i].contains("role")) {
                OATPP_LOGi("chat", "Message {}: role={}", i, inputs.messages[i]["role"].dump());
                if (inputs.messages[i].contains("tool_calls")) {
                    OATPP_LOGi("chat", "Message {} has tool_calls: {}", i, inputs.messages[i]["tool_calls"].dump());
                }
                if (inputs.messages[i].contains("name")) {
                    OATPP_LOGi("chat", "Message {} has name: {}", i, inputs.messages[i]["name"].dump());
                }
            }
        }
    } catch (const json::parse_error& e) {
        OATPP_LOGe("chat", "Failed to parse messages JSON: {} at position {} (line {}, column {})", 
                   e.what(), e.byte, e.id, e.byte);
        
        // Log the problematic region
        const auto messages_str = m_contentMappers->getDefaultMapper()
                                    ->writeToString(generation_params->messages)
                                    .getValue("");
        if (messages_str.length() > e.byte) {
            size_t start = (e.byte > 100) ? e.byte - 100 : 0;
            size_t len = std::min(static_cast<size_t>(200), messages_str.length() - start);
            OATPP_LOGe("chat", "JSON around error position {}: {}", e.byte, messages_str.substr(start, len));
        }
        
        auto error_result = ErrorResponse::createShared();
        error_result->error = "Failed to parse messages: " + std::string(e.what()) + 
                             " at position " + std::to_string(e.byte);
        return createDtoResponse(Status::CODE_400, error_result);
    } catch (const std::exception& e) {
        OATPP_LOGe("chat", "Error processing messages: {}", e.what());
        auto error_result = ErrorResponse::createShared();
        error_result->error = "Error processing messages: " + std::string(e.what());
        return createDtoResponse(Status::CODE_400, error_result);
    }
    
    // Extract and parse tools if provided
    if (generation_params->tools) {
        const auto tools_str = m_contentMappers->getDefaultMapper()
                                   ->writeToString(generation_params->tools)
                                   .getValue("");
        OATPP_LOGi("chat", "Tools string from DTO: {}", tools_str);
        if (!tools_str.empty() && tools_str != "null") {
            try {
                auto parsed_tools = json::parse(tools_str);
                OATPP_LOGi("chat", "Successfully parsed {} tools", parsed_tools.size());
                
                // Transform Ollama format tools to the format expected by templates
                // Ollama format: [{"type":"function","function":{"name":"...","description":"...","parameters":{}}}]
                // Template expects: array of tool objects (can be in various formats)
                inputs.tools = json::array();
                if (parsed_tools.is_array()) {
                    for (const auto& tool : parsed_tools) {
                        // If tool has "function" nested, extract it; otherwise use as-is
                        if (tool.is_object() && tool.contains("function")) {
                            // Extract the function object which contains name, description, parameters
                            inputs.tools.push_back(tool["function"]);
                        } else {
                            // Use tool as-is
                            inputs.tools.push_back(tool);
                        }
                    }
                }
                OATPP_LOGi("chat", "Transformed to {} tools for template", inputs.tools.size());
                if (inputs.tools.is_array() && inputs.tools.size() > 0) {
                    OATPP_LOGi("chat", "First transformed tool: {}", inputs.tools[0].dump());
                }
            } catch (const json::parse_error& e) {
                OATPP_LOGw("chat", "Failed to parse tools JSON: {} - Raw string: {}", e.what(), tools_str);
                inputs.tools = json::array();  // Default to empty array on parse error
            }
        } else {
            OATPP_LOGw("chat", "Tools string is empty or null");
            inputs.tools = json::array();
        }
    } else {
        OATPP_LOGi("chat", "No tools field in request");
        inputs.tools = json::array();  // Default to empty array if not provided
    }

    const std::string prompt_templ = templ.apply(inputs);
    OATPP_LOGi("chat", "Generated prompt length: {} chars", prompt_templ.length());
    // Log a snippet of the prompt to verify tools are included
    if (prompt_templ.find("<tools>") != std::string::npos) {
        size_t tools_start = prompt_templ.find("<tools>");
        size_t tools_end = prompt_templ.find("</tools>", tools_start);
        if (tools_end != std::string::npos) {
            std::string tools_section = prompt_templ.substr(tools_start, tools_end + 8 - tools_start);
            OATPP_LOGi("chat", "Tools section in prompt: {}", tools_section);
        }
    }

    return handle_completion(
        model_data,
        prompt_templ,
        generation_params->options,
        stream,
        generation_params->keep_alive,
        model,
        ReturnType::MESSAGE
    );
}

std::shared_ptr<oat::OutgoingResponse> MyController::chat_completions(
    const oatpp::Object<CreateChatCompletionParams>& generation_params
) {
    if (!generation_params->messages) {
        auto error_result = ErrorResponse::createShared();
        error_result->error = "messages field is required";
        return createDtoResponse(Status::CODE_200, error_result);
    }
    if (generation_params->n != 1) {
        auto error_result = ErrorResponse::createShared();
        error_result->error = "only n == 1 is supported";
        return createDtoResponse(Status::CODE_200, error_result);
    }
    if (generation_params->stream) {
        auto error_result = ErrorResponse::createShared();
        error_result->error =
            "streaming not supported on this endpoint. Use /api/chat endpoint instead.";
        return createDtoResponse(Status::CODE_200, error_result);
    }
    const auto& model = generation_params->model;
    const auto model_data_opt = get_model_data(model);
    if (!model_data_opt) {
        auto error_result = ErrorResponse::createShared();
        error_result->error = "model '" + model + "' not found";
        return createDtoResponse(Status::CODE_200, error_result);
    }
    const auto& model_data = model_data_opt->first;

    minja::chat_template templ(
        model_data.template_params.chat_template,
        model_data.template_params.bos_token,
        model_data.template_params.eos_token
    );

    const auto stream = generation_params->stream;

    minja::chat_template_inputs inputs;
    inputs.add_generation_prompt = true;
    inputs.messages =
        json::parse(m_contentMappers->getDefaultMapper()
                        ->writeToString(generation_params->messages)
                        .getValue(""));

    const std::string prompt_templ = templ.apply(inputs);

    auto model_options = ModelParameters::createShared();
    model_options->temperature = generation_params->temperature;
    model_options->seed = generation_params->seed;
    model_options->top_p = generation_params->top_p;
    model_options->frequency_penalty = generation_params->frequency_penalty;
    if (generation_params->max_tokens) {
        model_options->num_predict = generation_params->max_tokens;
    }
    if (generation_params->max_completion_tokens) {
        model_options->num_predict = generation_params->max_completion_tokens;
    }

    return handle_completion(
        model_data,
        prompt_templ,
        model_options,
        stream,
        oatpp::Int32(nullptr),
        model,
        ReturnType::COMPLETION
    );
}
