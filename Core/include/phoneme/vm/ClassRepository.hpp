#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "phoneme/archive/ZipArchive.hpp"
#include "phoneme/classfile/ClassFile.hpp"
#include "phoneme/vm/RuntimeMetadata.hpp"

namespace phoneme::vm {

struct ResolvedMethod final {
    std::shared_ptr<const classfile::ClassFile> owner;
    const classfile::Method* method {nullptr};
    std::shared_ptr<const RuntimeMethod> runtime;
};

class ClassRepository final {
public:
    [[nodiscard]] Status add_archive(std::string archive_path);
    [[nodiscard]] Status add_archive(
        std::string archive_path,
        const std::array<u8, 32>& archive_sha256,
        u32 verification_cache_version,
        const std::unordered_map<std::string, u64>& verified_classes);
    [[nodiscard]] Result<std::shared_ptr<const classfile::ClassFile>> load(
        std::string_view binary_name);
    [[nodiscard]] Result<ResolvedMethod> resolve_method(
        std::string_view binary_name,
        std::string_view method_name,
        std::string_view descriptor);
    [[nodiscard]] Result<ResolvedMethod> resolve_declared_method(
        std::string_view binary_name,
        std::string_view method_name,
        std::string_view descriptor);
    [[nodiscard]] Result<bool> is_assignable(
        std::string_view source_name,
        std::string_view target_name);
    [[nodiscard]] Result<std::vector<u8>> read_resource(
        std::string_view resource_name) const;
    [[nodiscard]] RuntimeMetadata& metadata() noexcept { return metadata_; }
    [[nodiscard]] const RuntimeMetadata& metadata() const noexcept {
        return metadata_;
    }
    [[nodiscard]] std::unordered_map<std::string, u64> verified_classes(
        std::string_view archive_path) const;
    [[nodiscard]] static constexpr u32 verification_cache_version() noexcept {
        return 1U;
    }
    void clear() noexcept;

private:
    struct StringHash final {
        using is_transparent = void;

        [[nodiscard]] usize operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
        [[nodiscard]] usize operator()(const std::string& value) const noexcept {
            return (*this)(std::string_view(value));
        }
    };

    struct StringEqual final {
        using is_transparent = void;

        [[nodiscard]] bool operator()(std::string_view left,
                                      std::string_view right) const noexcept {
            return left == right;
        }
    };

    struct MethodCacheKey final {
        std::string owner;
        std::string name;
        std::string descriptor;
    };

    struct MethodCacheKeyView final {
        std::string_view owner;
        std::string_view name;
        std::string_view descriptor;
    };

    struct MethodCacheKeyHash final {
        using is_transparent = void;
        [[nodiscard]] usize operator()(const MethodCacheKey& key) const noexcept;
        [[nodiscard]] usize operator()(MethodCacheKeyView key) const noexcept;
    };

    struct MethodCacheKeyEqual final {
        using is_transparent = void;
        [[nodiscard]] bool operator()(const MethodCacheKey& left,
                                      const MethodCacheKey& right) const noexcept;
        [[nodiscard]] bool operator()(const MethodCacheKey& left,
                                      MethodCacheKeyView right) const noexcept;
        [[nodiscard]] bool operator()(MethodCacheKeyView left,
                                      const MethodCacheKey& right) const noexcept;
    };

    struct AssignabilityCacheKey final {
        std::string source;
        std::string target;
    };

    struct AssignabilityCacheKeyView final {
        std::string_view source;
        std::string_view target;
    };

    struct AssignabilityCacheKeyHash final {
        using is_transparent = void;
        [[nodiscard]] usize operator()(
            const AssignabilityCacheKey& key) const noexcept;
        [[nodiscard]] usize operator()(AssignabilityCacheKeyView key) const noexcept;
    };

    struct AssignabilityCacheKeyEqual final {
        using is_transparent = void;
        [[nodiscard]] bool operator()(const AssignabilityCacheKey& left,
                                      const AssignabilityCacheKey& right) const noexcept;
        [[nodiscard]] bool operator()(const AssignabilityCacheKey& left,
                                      AssignabilityCacheKeyView right) const noexcept;
        [[nodiscard]] bool operator()(AssignabilityCacheKeyView left,
                                      const AssignabilityCacheKey& right) const noexcept;
    };

    struct ClasspathArchive final {
        std::string path;
        archive::ZipArchive archive;
        std::array<u8, 32> archive_sha256 {};
        u32 verification_cache_version {0};
        std::unordered_map<std::string, u64> verified_classes;
        bool verification_cache_enabled {false};
    };

    [[nodiscard]] Status add_archive_impl(
        std::string archive_path,
        const std::array<u8, 32>* archive_sha256,
        u32 verification_cache_version,
        const std::unordered_map<std::string, u64>* verified_classes);
    [[nodiscard]] Result<std::shared_ptr<const classfile::ClassFile>> load_uncached(
        std::string_view internal_name);
    [[nodiscard]] static std::string normalize_name(std::string_view binary_name);

    mutable std::mutex mutex_;
    std::vector<ClasspathArchive> archives_;
    std::unordered_map<std::string,
                       std::shared_ptr<const classfile::ClassFile>,
                       StringHash,
                       StringEqual> cache_;
    std::unordered_map<MethodCacheKey,
                       ResolvedMethod,
                       MethodCacheKeyHash,
                       MethodCacheKeyEqual> method_cache_;
    std::unordered_map<MethodCacheKey,
                       ResolvedMethod,
                       MethodCacheKeyHash,
                       MethodCacheKeyEqual> declared_method_cache_;
    std::unordered_map<AssignabilityCacheKey,
                       bool,
                       AssignabilityCacheKeyHash,
                       AssignabilityCacheKeyEqual> assignability_cache_;
    RuntimeMetadata metadata_;
};

} // namespace phoneme::vm
