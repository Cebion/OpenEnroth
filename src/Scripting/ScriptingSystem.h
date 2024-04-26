#pragma once

#include <string_view>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <utility>
#include <sol/sol.hpp>

class IBindings;

class ScriptingSystem {
 public:
    ScriptingSystem(
        std::string_view scriptFolder,
        std::string_view entryPointFile
    );

    void executeEntryPoints();

    template<typename TBindings, typename ...TArgs>
    void addBindings(std::string_view bindingTableName, TArgs &&... args) {
        auto bindings = std::make_unique<TBindings>(_solState, std::forward<TArgs>(args) ...);
        _addBindings(bindingTableName, std::move(bindings));
    }

 private:
    void _initBaseLibraries();
    void _initPackageTable(std::string_view scriptFolder);
    void _addBindings(std::string_view name, std::unique_ptr<IBindings> bindings);

    sol::state _solState;
    std::unordered_map<std::string, std::unique_ptr<IBindings>> _bindings;
    std::string _scriptFolder;
    std::string _entryPointFile;
};
