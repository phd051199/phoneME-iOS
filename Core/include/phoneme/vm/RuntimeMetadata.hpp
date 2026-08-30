#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "phoneme/classfile/ClassFile.hpp"
#include "phoneme/vm/DecodedMethod.hpp"
#include "phoneme/vm/Descriptor.hpp"
#include "phoneme/vm/MetadataId.hpp"
#include "phoneme/vm/Verifier.hpp"

namespace phoneme::vm {

enum class OperandResolutionState : u8 {
    unresolved,
    resolving,
    resolved,
    failed,
};

enum class OperandResolutionKind : u8 {
    none,
    field,
    direct_call,
    virtual_call,
    class_reference,
};

struct OperandResolutionEntry final {
    u32 bytecode_pc {kInvalidDecodedIndex};
    OperandResolutionState state {OperandResolutionState::unresolved};
    OperandResolutionKind kind {OperandResolutionKind::none};
    MethodId target_method;
    ClassId receiver_class;
    NativeMethodId target_native_method;
    u64 native_generation {0U};
    bool native_binding_cached {false};
    ClassId target_class;
    std::shared_ptr<const classfile::ClassFile> target_class_file;
    std::string target_class_name;
    std::string target_array_name;
    std::optional<Error> failure;

    [[nodiscard]] Status begin(OperandResolutionKind expected_kind);
    [[nodiscard]] Status resolve_direct_call(
        MethodId resolved_method,
        NativeMethodId resolved_native_method,
        u64 resolved_native_generation);
    [[nodiscard]] Status begin_virtual_call(ClassId resolved_receiver_class);
    [[nodiscard]] Status resolve_virtual_call(
        MethodId resolved_method,
        NativeMethodId resolved_native_method,
        u64 resolved_native_generation);
    [[nodiscard]] Status update_native_binding(
        NativeMethodId resolved_native_method,
        u64 resolved_native_generation);
    [[nodiscard]] Status resolve_class_reference(
        std::string resolved_class_name,
        ClassId resolved_class,
        std::shared_ptr<const classfile::ClassFile> resolved_class_file,
        std::string resolved_array_name = {});
    [[nodiscard]] Status fail_resolution(Error error);
    void reset() noexcept;
};

class OperandResolutionTable final {
public:
    explicit OperandResolutionTable(const DecodedMethod& decoded);

    [[nodiscard]] Result<OperandResolutionEntry*> entry(
        u32 operand_index,
        u32 bytecode_pc) noexcept;
    [[nodiscard]] const OperandResolutionEntry* entry_for_test(
        u32 operand_index) const noexcept;
    [[nodiscard]] usize size() const noexcept { return entries_.size(); }

private:
    std::vector<OperandResolutionEntry> entries_;
};

struct CachedMethodDescriptor final {
    MethodDescriptor descriptor;
    usize argument_values {0};
    usize argument_slots_without_receiver {0};
    usize argument_slots_with_receiver {0};
    JavaTypeKind return_kind {JavaTypeKind::void_type};
    bool returns_category_two {false};

    [[nodiscard]] usize argument_slots(bool has_receiver) const noexcept {
        return has_receiver ? argument_slots_with_receiver
                            : argument_slots_without_receiver;
    }
};

struct RuntimeClass final {
    ClassId id;
    std::shared_ptr<const classfile::ClassFile> class_file;
    std::string super_name;
    ClassId super_id;
    std::vector<std::string> interface_names;
    std::vector<ClassId> interface_ids;
};

struct RuntimeMethod final {
    MethodId id;
    ClassId declaring_class;
    std::shared_ptr<const classfile::ClassFile> owner;
    const classfile::Method* method {nullptr};
    std::shared_ptr<const CachedMethodDescriptor> descriptor;
    std::shared_ptr<const DecodedMethod> decoded;
    std::shared_ptr<OperandResolutionTable> operand_resolutions;
    std::shared_ptr<const VerifiedMethodReferenceMaps> verified_frames;
    // Decoded instruction index -> verified frame index. Unreachable decoded
    // instructions retain kInvalidDecodedIndex. This avoids a binary search by
    // bytecode PC at every interpreter safepoint.
    std::vector<u32> verified_frame_index_by_instruction;

    // Native/HLE binding is logically derived metadata. Cache both hits and
    // misses directly on the immutable RuntimeMethod identity so steady-state
    // invocation does not take Machine's native-binding mutex merely to read
    // a dense MethodId slot. Publish the id before the generation; readers
    // acquire the generation and can then consume the matching id lock-free.
    mutable std::atomic<u32> cached_native_method_id {0U};
    mutable std::atomic<u64> cached_native_generation {0U};
};

struct RuntimeMetadataDiagnostics final {
    usize classes {0U};
    usize methods {0U};
    usize descriptors {0U};
    usize decoded_instructions {0U};
    usize decoded_operands {0U};
};

class RuntimeMetadata final {
public:
    RuntimeMetadata() = default;

    RuntimeMetadata(const RuntimeMetadata&) = delete;
    RuntimeMetadata& operator=(const RuntimeMetadata&) = delete;

    [[nodiscard]] Result<std::shared_ptr<const RuntimeClass>> publish_class(
        std::shared_ptr<const classfile::ClassFile> class_file);
    [[nodiscard]] Result<std::shared_ptr<const RuntimeMethod>> publish_method(
        const std::shared_ptr<const classfile::ClassFile>& owner,
        const classfile::Method& method);
    [[nodiscard]] Result<std::shared_ptr<const CachedMethodDescriptor>>
    method_descriptor(std::string_view descriptor);

    [[nodiscard]] std::shared_ptr<const RuntimeClass> find_class(
        std::string_view internal_name) const noexcept;
    [[nodiscard]] std::shared_ptr<const RuntimeClass> find_class(
        ClassId id) const noexcept;
    [[nodiscard]] std::shared_ptr<const RuntimeMethod> find_method(
        const classfile::ClassFile* owner,
        const classfile::Method* method) const noexcept;
    [[nodiscard]] std::shared_ptr<const RuntimeMethod> find_method(
        MethodId id) const noexcept;

    [[nodiscard]] u64 generation() const noexcept;
    [[nodiscard]] RuntimeMetadataDiagnostics diagnostics() const noexcept;
    void clear() noexcept;

private:
    struct TransparentStringHash final {
        using is_transparent = void;

        [[nodiscard]] usize operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
        [[nodiscard]] usize operator()(const std::string& value) const noexcept {
            return (*this)(std::string_view(value));
        }
    };

    struct TransparentStringEqual final {
        using is_transparent = void;

        [[nodiscard]] bool operator()(std::string_view left,
                                      std::string_view right) const noexcept {
            return left == right;
        }
    };

    struct MethodPointerKey final {
        const classfile::ClassFile* owner {nullptr};
        const classfile::Method* method {nullptr};

        friend bool operator==(MethodPointerKey,
                               MethodPointerKey) noexcept = default;
    };

    struct MethodPointerKeyHash final {
        [[nodiscard]] usize operator()(MethodPointerKey key) const noexcept;
    };

    [[nodiscard]] static Result<std::shared_ptr<const CachedMethodDescriptor>>
    parse_cached_descriptor(std::string_view descriptor);
    [[nodiscard]] ClassId class_id_unlocked(
        std::string_view internal_name) const noexcept;
    void advance_generation_unlocked() noexcept;

    mutable std::mutex mutex_;
    std::atomic<u64> generation_ {1U};
    u32 next_class_id_ {1};
    u32 next_method_id_ {1};
    std::unordered_map<std::string,
                       std::shared_ptr<const RuntimeClass>,
                       TransparentStringHash,
                       TransparentStringEqual> classes_by_name_;
    std::unordered_map<const classfile::ClassFile*,
                       std::shared_ptr<const RuntimeClass>> classes_by_pointer_;
    std::vector<std::shared_ptr<const RuntimeClass>> classes_by_id_;
    std::unordered_map<MethodPointerKey,
                       std::shared_ptr<const RuntimeMethod>,
                       MethodPointerKeyHash> methods_;
    std::vector<std::shared_ptr<const RuntimeMethod>> methods_by_id_;
    std::unordered_map<std::string,
                       std::shared_ptr<const CachedMethodDescriptor>,
                       TransparentStringHash,
                       TransparentStringEqual> descriptors_;
};

} // namespace phoneme::vm
