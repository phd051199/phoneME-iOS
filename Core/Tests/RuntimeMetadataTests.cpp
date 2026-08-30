#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

#include "phoneme/classfile/ClassFile.hpp"
#include "phoneme/vm/ClassLayout.hpp"
#include "phoneme/vm/RuntimeMetadata.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

phoneme::classfile::Method method(std::string name,
                                  std::string descriptor) {
    return phoneme::classfile::Method {
        .access_flags = 0x0009U,
        .name = std::move(name),
        .descriptor = std::move(descriptor),
        .code = std::nullopt,
    };
}

} // namespace

int main() {
    using phoneme::classfile::ClassFile;
    using phoneme::vm::RuntimeMetadata;

    std::vector<phoneme::classfile::Method> methods;
    methods.push_back(method("sum", "(II)I"));
    methods.push_back(method("wide", "(IDLjava/lang/Object;)J"));
    methods.push_back(method("empty", "()V"));
    methods.push_back(phoneme::classfile::Method {
        .access_flags = 0x0009U,
        .name = "coded",
        .descriptor = "()I",
        .code = phoneme::classfile::CodeAttribute {
            .max_stack = 1U,
            .max_locals = 0U,
            .bytecode = {0x04U, 0xACU},
        },
    });
    methods.push_back(phoneme::classfile::Method {
        .access_flags = 0x0009U,
        .name = "linked",
        .descriptor = "()V",
        .code = phoneme::classfile::CodeAttribute {
            .max_stack = 1U,
            .max_locals = 0U,
            .bytecode = {
                0xB2U, 0x00U, 0x01U,
                0xB8U, 0x00U, 0x02U,
                0xB1U,
            },
        },
    });

    auto class_file = std::make_shared<const ClassFile>(ClassFile::builtin(
        "test/Metadata",
        "java/lang/Object",
        0x0001U,
        {},
        std::move(methods)));

    const auto* sum = class_file->find_method("sum", "(II)I");
    const auto* wide = class_file->find_method(
        "wide", "(IDLjava/lang/Object;)J");
    const auto* coded = class_file->find_method("coded", "()I");
    const auto* linked = class_file->find_method("linked", "()V");
    require(sum != nullptr, "indexed method lookup finds sum");
    require(wide != nullptr, "indexed method lookup finds wide");
    require(coded != nullptr, "indexed method lookup finds coded method");
    require(linked != nullptr, "indexed method lookup finds linked method");
    require(class_file->find_method("missing", "()V") == nullptr,
            "indexed method lookup rejects missing method");

    RuntimeMetadata metadata;
    const auto initial_generation = metadata.generation();
    auto object_class = std::make_shared<const ClassFile>(ClassFile::builtin(
        "java/lang/Object", "", 0x0001U, {}, {}));
    auto runtime_object = metadata.publish_class(object_class);
    require(runtime_object.has_value(), "publish runtime Object class");
    auto runtime_class = metadata.publish_class(class_file);
    require(runtime_class.has_value(), "publish runtime class");
    require((*runtime_class)->id.valid(), "runtime class has valid ID");
    require((*runtime_class)->class_file.get() == class_file.get(),
            "runtime class retains stable class file");
    require((*runtime_class)->super_name == "java/lang/Object" &&
                (*runtime_class)->super_id == (*runtime_object)->id,
            "runtime class retains superclass metadata");
    require((*runtime_class)->interface_names.empty() &&
                (*runtime_class)->interface_ids.empty(),
            "runtime class retains interface metadata");

    auto sum_metadata = metadata.publish_method(class_file, *sum);
    auto wide_metadata = metadata.publish_method(class_file, *wide);
    auto coded_metadata = metadata.publish_method(class_file, *coded);
    auto linked_metadata = metadata.publish_method(class_file, *linked);
    require(sum_metadata.has_value(), "publish sum method");
    require(wide_metadata.has_value(), "publish wide method");
    require(coded_metadata.has_value(), "publish coded method");
    require(linked_metadata.has_value(), "publish linked method");
    require((*sum_metadata)->id.valid() && (*wide_metadata)->id.valid(),
            "runtime methods have valid IDs");
    require((*sum_metadata)->id != (*wide_metadata)->id,
            "runtime method IDs are unique");
    require((*sum_metadata)->decoded == nullptr,
            "method without Code has no decoded body");
    require((*coded_metadata)->decoded != nullptr &&
                (*coded_metadata)->decoded->method_id == (*coded_metadata)->id &&
                (*coded_metadata)->decoded->instructions.size() == 2U,
            "runtime method publishes immutable decoded bytecode");
    require((*coded_metadata)->verified_frames != nullptr &&
                !(*coded_metadata)->verified_frames->frames.empty() &&
                (*coded_metadata)->verified_frames->max_stack == 1U,
            "runtime method retains immutable verifier slot/reference maps");
    require((*linked_metadata)->decoded != nullptr &&
                (*linked_metadata)->operand_resolutions != nullptr &&
                (*linked_metadata)->operand_resolutions->size() == 2U,
            "runtime method owns a decoded operand side table");

    auto operand_entry = (*linked_metadata)->operand_resolutions->entry(0U, 0U);
    require(operand_entry.has_value(),
            "decoded operand side-table entry uses original BCI");
    require(!(*linked_metadata)->operand_resolutions->entry(0U, 3U).has_value(),
            "operand side table rejects a mismatched BCI");
    // FieldLocation contains Machine-local FieldId state and is deliberately
    // excluded from shared RuntimeMetadata. Field linkage is cached by Machine.
    require((*operand_entry)->begin(
                phoneme::vm::OperandResolutionKind::class_reference).has_value(),
            "class operand enters resolving state");
    require((*operand_entry)->resolve_class_reference(
                "test/Metadata",
                (*runtime_class)->id,
                class_file,
                "[Ltest/Metadata;").has_value() &&
                (*operand_entry)->target_class == (*runtime_class)->id &&
                (*operand_entry)->target_class_file == class_file &&
                (*operand_entry)->target_class_name == "test/Metadata" &&
                (*operand_entry)->target_array_name == "[Ltest/Metadata;",
            "class operand publishes stable class and array metadata");
    (*operand_entry)->reset();
    require((*operand_entry)->begin_virtual_call((*runtime_class)->id)
                .has_value(),
            "virtual-call operand binds its receiver class");
    require((*operand_entry)->resolve_virtual_call(
                (*sum_metadata)->id,
                phoneme::vm::NativeMethodId {9U},
                13U).has_value() &&
                (*operand_entry)->kind ==
                    phoneme::vm::OperandResolutionKind::virtual_call &&
                (*operand_entry)->receiver_class == (*runtime_class)->id &&
                (*operand_entry)->target_method == (*sum_metadata)->id &&
                (*operand_entry)->target_native_method ==
                    phoneme::vm::NativeMethodId {9U},
            "virtual-call operand publishes a monomorphic method/native target");

    auto call_entry = (*linked_metadata)->operand_resolutions->entry(1U, 3U);
    require(call_entry.has_value(), "direct-call operand uses original BCI");
    require((*call_entry)->begin(
                phoneme::vm::OperandResolutionKind::direct_call).has_value(),
            "direct-call operand enters resolving state");
    require((*call_entry)->resolve_direct_call(
                (*sum_metadata)->id,
                phoneme::vm::NativeMethodId {7U},
                11U).has_value() &&
                (*call_entry)->target_method == (*sum_metadata)->id &&
                (*call_entry)->target_native_method ==
                    phoneme::vm::NativeMethodId {7U} &&
                (*call_entry)->native_generation == 11U &&
                (*call_entry)->native_binding_cached,
            "direct-call operand publishes method and native target IDs");
    require((*call_entry)->update_native_binding(
                phoneme::vm::NativeMethodId {}, 12U).has_value() &&
                !(*call_entry)->target_native_method.valid() &&
                (*call_entry)->native_generation == 12U,
            "direct-call operand caches a generation-scoped missing native");
    (*call_entry)->reset();
    require((*call_entry)->begin(
                phoneme::vm::OperandResolutionKind::direct_call).has_value(),
            "direct-call operand can be reset for a new linkage attempt");
    const auto stable_failure = phoneme::Error::make_java(
        "java/lang/NoSuchMethodError", "test/Metadata.missing()V");
    require((*call_entry)->fail_resolution(stable_failure).has_value(),
            "direct-call operand caches a linkage failure");
    const auto* cached_failure =
        (*linked_metadata)->operand_resolutions->entry_for_test(1U);
    require(cached_failure != nullptr &&
                cached_failure->state ==
                    phoneme::vm::OperandResolutionState::failed &&
                cached_failure->failure.has_value() &&
                cached_failure->failure->java_exception_class ==
                    "java/lang/NoSuchMethodError" &&
                cached_failure->failure->message ==
                    "test/Metadata.missing()V",
            "cached linkage failure preserves throwable class and message");
    require(metadata.publish_method(class_file, *sum).value().get() ==
                sum_metadata->get(),
            "publishing a method twice reuses metadata");

    auto descriptor = metadata.method_descriptor(
        "(IDLjava/lang/Object;)J");
    auto descriptor_again = metadata.method_descriptor(
        "(IDLjava/lang/Object;)J");
    require(descriptor.has_value() && descriptor_again.has_value(),
            "cache parsed method descriptor");
    require(descriptor->get() == descriptor_again->get(),
            "descriptor cache returns stable object");
    require((*descriptor)->argument_values == 3U,
            "descriptor caches argument value count");
    require((*descriptor)->argument_slots_without_receiver == 4U,
            "descriptor caches category-two argument slots");
    require((*descriptor)->argument_slots_with_receiver == 5U,
            "descriptor caches receiver slot");
    require((*descriptor)->returns_category_two,
            "descriptor caches category-two return kind");

    require(metadata.find_class("test/Metadata").get() ==
                runtime_class->get(),
            "find class by internal name");
    require(metadata.find_class((*runtime_class)->id).get() ==
                runtime_class->get(),
            "find class by stable ClassId");
    require(metadata.find_method(class_file.get(), sum).get() ==
                sum_metadata->get(),
            "find method by stable pointers");
    require(metadata.find_method((*sum_metadata)->id).get() ==
                sum_metadata->get(),
            "find method by stable MethodId");

    metadata.clear();
    require(metadata.generation() != initial_generation,
            "clear advances metadata generation");
    require(metadata.find_class("test/Metadata") == nullptr,
            "clear invalidates class publication");
    require(metadata.find_method(class_file.get(), sum) == nullptr,
            "clear invalidates method publication");

    std::cout << "Runtime metadata tests passed\n";
    return 0;
}
