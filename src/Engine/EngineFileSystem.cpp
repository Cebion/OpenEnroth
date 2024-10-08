#include "EngineFileSystem.h"

#include <memory>
#include <vector>

#include "Library/FileSystem/Directory/DirectoryFileSystem.h"
#include "Library/FileSystem/Embedded/EmbeddedFileSystem.h"
#include "Library/FileSystem/Lowercase/LowercaseFileSystem.h"
#include "Library/FileSystem/Merging/MergingFileSystem.h"
#include "Library/FileSystem/Masking/MaskingFileSystem.h"

FileSystem *dfs = nullptr;
FileSystem *ufs = nullptr;

CMRC_DECLARE(openenroth);

EngineFileSystem::EngineFileSystem(std::string_view dataPath, std::string_view userPath) {
    _dataEmbeddedFs = std::make_unique<EmbeddedFileSystem>(cmrc::openenroth::get_filesystem(), "embedded");
    _dataDirFs = std::make_unique<DirectoryFileSystem>(dataPath);
    _dataDirLowercaseFs = std::make_unique<LowercaseFileSystem>(_dataDirFs.get());
    _defaultDataFs = std::make_unique<MergingFileSystem>(std::vector<const FileSystem *>({_dataDirLowercaseFs.get(), _dataEmbeddedFs.get()}));

    _defaultUserFs = std::make_unique<DirectoryFileSystem>(userPath);

    assert(dfs == nullptr && ufs == nullptr);
    dfs = _defaultDataFs.get();
    ufs = _defaultUserFs.get();
}

EngineFileSystem::~EngineFileSystem() {
    dfs = nullptr;
    ufs = nullptr;
}
