#pragma once

#include <cstdint>
#include <string>
#include <vector>

class CESLocalGroupPolicyManager {
public:
    enum class Scope {
        Machine,
        User,
    };

    enum class ValueType {
        Dword,
        Qword,
        String,
        ExpandString,
        MultiString,
        Binary,
    };

    struct PolicyValue {
        std::wstring keyPath;
        std::wstring valueName;
        ValueType type{ValueType::String};
        std::uint32_t dwordValue{};
        std::uint64_t qwordValue{};
        std::wstring stringValue;
        std::vector<std::wstring> multiStringValue;
        std::vector<std::uint8_t> binaryValue;
    };

    CESLocalGroupPolicyManager() = default;
    ~CESLocalGroupPolicyManager() = default;

    CESLocalGroupPolicyManager(CESLocalGroupPolicyManager const&) = default;
    CESLocalGroupPolicyManager& operator=(CESLocalGroupPolicyManager const&) = default;
    CESLocalGroupPolicyManager(CESLocalGroupPolicyManager&&) noexcept = default;
    CESLocalGroupPolicyManager& operator=(CESLocalGroupPolicyManager&&) noexcept = default;

    static std::wstring BuildPolicyKeyPath(std::wstring relativeKeyPath);

    bool SetPolicyValue(Scope scope, PolicyValue const& value, std::wstring* errorMessage = nullptr) const;
    bool TryGetPolicyValue(
        Scope scope,
        std::wstring const& relativeKeyPath,
        std::wstring const& valueName,
        PolicyValue& value,
        std::wstring* errorMessage = nullptr) const;
    bool DeletePolicyValue(
        Scope scope,
        std::wstring const& relativeKeyPath,
        std::wstring const& valueName,
        std::wstring* errorMessage = nullptr) const;
    bool DeletePolicyKey(Scope scope, std::wstring const& relativeKeyPath, std::wstring* errorMessage = nullptr) const;
    bool EnumeratePolicyValues(
        Scope scope,
        std::wstring const& relativeKeyPath,
        std::vector<PolicyValue>& values,
        std::wstring* errorMessage = nullptr) const;
    bool ApplyPolicies(bool updateMachine, bool updateUser, std::wstring* errorMessage = nullptr) const;
};
