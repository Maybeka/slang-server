// SPDX-License-Identifier: MIT
#pragma once
#include <rfl/from_generic.hpp>
#include <rfl/generic/Writer.hpp>
#include <rfl/parsing/Parser.hpp>
#include <variant>

namespace server::detail {
// Input nodes borrow the request-owned Generic tree for this synchronous read.
// Typed results still own their strings. Cancellation-key validation can erase
// the *value* of text fields after checking their type, without copying megabytes
// of source that are irrelevant to the routing key.
struct BorrowedGenericReader {
    struct InputVarType {
        const rfl::Generic* value = nullptr;
        bool eraseString = false;
    };
    using InputArrayType = const rfl::Generic::Array*;
    using InputObjectType = const rfl::Generic::Object*;
    bool eraseText = false;
    bool* ambiguous = nullptr;
    template<class T> static constexpr bool has_custom_constructor = false;

    rfl::Result<InputVarType> get_field_from_array(size_t index, InputArrayType array) const noexcept {
        if (index >= array->size())
            return rfl::error("Index " + std::to_string(index) + " of of bounds.");
        return InputVarType{&(*array)[index], false};
    }
    rfl::Result<InputVarType> get_field_from_object(const std::string& name,
                                                  InputObjectType object) const noexcept {
        for (const auto& [key, value] : *object)
            if (key == name)
                return InputVarType{&value, eraseText && key == "text"};
        return rfl::error("Key named '" + name + "' not found.");
    }
    bool is_empty(const InputVarType& var) const noexcept {
        return !var.value || var.value->is_null();
    }
    template<class T> rfl::Result<T> to_basic_type(const InputVarType& var) const noexcept {
        static const rfl::Generic empty;
        const auto& value = var.value ? *var.value : empty;
        if constexpr (std::is_same_v<T, std::string>)
            if (const auto* text = std::get_if<std::string>(&value.get()))
                return var.eraseString ? std::string{} : *text;
        // Retain the library's exact numeric conversions and error messages.
        return rfl::generic::Reader{}.template to_basic_type<T>(value);
    }
    template<class ArrayReader>
    std::optional<rfl::Error> read_array(const ArrayReader& reader,
                                        InputArrayType array) const noexcept {
        for (const auto& item : *array) {
            auto var = InputVarType{&item, false};
            if (auto error = reader.read(var))
                return error;
        }
        return {};
    }
    template<class ObjectReader>
    std::optional<rfl::Error> read_object(const ObjectReader& reader,
                                         InputObjectType object) const noexcept {
        for (const auto& [key, value] : *object) {
            auto var = InputVarType{&value, eraseText && key == "text"};
            reader.read(std::string_view(key), var);
        }
        return {};
    }
    rfl::Result<InputArrayType> to_array(const InputVarType& var) const noexcept {
        if (var.value)
            if (auto* array = std::get_if<rfl::Generic::Array>(&var.value->get()))
                return array;
        return rfl::error("rfl::Generic: Could not cast the underlying value to an "
                          "rfl::Generic::Array.");
    }
    rfl::Result<InputObjectType> to_object(const InputVarType& var) const noexcept {
        if (var.value)
            if (auto* object = std::get_if<rfl::Generic::Object>(&var.value->get())) {
                // The owning Reader has an internal lookup cursor. Duplicate
                // names can depend on that cursor; preserve its exact behavior
                // by using the old reader for this entire exceptional payload.
                for (auto a = object->begin(); a != object->end(); ++a)
                    for (auto b = object->begin(); b != a; ++b)
                        if (a->first == b->first) {
                            if (ambiguous) *ambiguous = true;
                            return rfl::error("ambiguous borrowed Generic object");
                        }
                return object;
            }
        return rfl::error("rfl::Generic: Could not cast the underlying value to an "
                          "rfl::Generic::Object.");
    }
    template<class T> rfl::Result<T> use_custom_constructor(InputVarType) const noexcept {
        return rfl::error("Not supported for generic types");
    }
};
} // namespace server::detail

namespace server {
template<class T, class... Processors>
auto readBorrowedGeneric(const rfl::Generic& source, bool eraseText = false) {
    bool ambiguous = false;
    const detail::BorrowedGenericReader reader{eraseText, &ambiguous};
    using Parser = rfl::parsing::Parser<detail::BorrowedGenericReader, rfl::generic::Writer,
                                       T, rfl::Processors<Processors...>>;
    auto result = Parser::read(reader, {&source, false});
    if (ambiguous)
        return rfl::from_generic<T, Processors...>(source);
    return result;
}
} // namespace server
