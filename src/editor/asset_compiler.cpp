#include "asset_compiler.h"
#include "editor/file_system_watcher.h"
#include "editor/platform_interface.h"
#include "editor/studio_app.h"
#include "editor/world_editor.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/fs/disk_file_device.h"
#include "engine/fs/os_file.h"
#include "engine/log.h"
#include "engine/mt/sync.h"
#include "engine/mt/task.h"
#include "engine/path_utils.h"
#include "engine/resource.h"
#include "engine/resource_manager.h"

namespace Lumix
{


struct AssetCompilerImpl;


struct AssetCompilerTask : MT::Task
{
	AssetCompilerTask(AssetCompilerImpl& compiler, IAllocator& allocator) 
		: MT::Task(allocator) 
		 , m_compiler(compiler)
	{}

	int task() override;

	AssetCompilerImpl& m_compiler;
	volatile bool m_finished = false;
};


struct AssetCompilerImpl : AssetCompiler
{
	struct CompileEntry
	{
		Path path;
		Resource* resource;
	};

	struct LoadHook : ResourceManager::LoadHook
	{
		LoadHook(AssetCompilerImpl& compiler) : compiler(compiler) {}

		bool onBeforeLoad(Resource& res) override
		{
			return compiler.onBeforeLoad(res);
		}

		AssetCompilerImpl& compiler;
	};


	AssetCompilerImpl(StudioApp& app) 
		: m_app(app)
		, m_load_hook(*this)
		, m_plugins(app.getWorldEditor().getAllocator())
		, m_task(*this, app.getWorldEditor().getAllocator())
		, m_to_compile(app.getWorldEditor().getAllocator())
		, m_compiled(app.getWorldEditor().getAllocator())
		, m_to_compile_mutex(false)
		, m_compiled_mutex(false)
		, m_plugin_mutex(false)
		, m_semaphore(0, 0x7fFFffFF)
	{
		m_watcher = FileSystemWatcher::create(".", app.getWorldEditor().getAllocator());
		m_watcher->getCallback().bind<AssetCompilerImpl, &AssetCompilerImpl::onFileChanged>(this);
		m_task.create("asset compiler");
		const char* base_path = m_app.getWorldEditor().getEngine().getDiskFileDevice()->getBasePath();
		StaticString<MAX_PATH_LENGTH> path(base_path, ".lumix/assets");
		PlatformInterface::makePath(path);
		ResourceManager& rm = app.getWorldEditor().getEngine().getResourceManager();
		rm.setLoadHook(&m_load_hook);
	}

	~AssetCompilerImpl()
	{
		m_task.m_finished = true;
		m_to_compile.emplace().resource = nullptr;
		m_semaphore.signal();
		m_task.destroy();
		ResourceManager& rm = m_app.getWorldEditor().getEngine().getResourceManager();
		rm.setLoadHook(nullptr);
		FileSystemWatcher::destroy(m_watcher);
	}


	void onFileChanged(const char* path)
	{
		if (startsWith(path, ".lumix")) return;
		
		char ext[16];
		PathUtils::getExtension(ext, lengthOf(ext), path);

		{
			MT::SpinLock lock(m_plugin_mutex);
			auto iter = m_plugins.find(crc32(ext));
			if(!iter.isValid()) return;
		}
		
		MT::SpinLock lock(m_to_compile_mutex);
		
		CompileEntry& e = m_to_compile.emplace();
		e.resource = nullptr;
		e.path = path;
		m_semaphore.signal();
	}


	int getMeta(const Path& res, void* buf, int size) const override
	{
		const PathUtils::FileInfo info(res.c_str());
		FS::OsFile file;
		const StaticString<MAX_PATH_LENGTH> meta_path(info.m_dir, info.m_basename, ".meta");
		
		if (!file.open(meta_path, FS::Mode::OPEN_AND_READ)) return -1;
		
		const int read_size = Math::minimum(size, (int)file.size());
		const bool success = file.read(buf, read_size);
		file.close();

		return success ? read_size : -1;
	}


	void updateMeta(const Path& res, const void* meta, int size) const override
	{
		const PathUtils::FileInfo info(res.c_str());
		FS::OsFile file;
		const StaticString<MAX_PATH_LENGTH> meta_path(info.m_dir, info.m_basename, ".meta");
				
		if (!file.open(meta_path, FS::Mode::CREATE_AND_WRITE)) {
			g_log_error.log("Editor") << "Could not create " << meta_path;
			return;
		}

		file.write(meta, size);
		file.close();
	}


	bool compile(const Path& src) override
	{
		char ext[16];
		PathUtils::getExtension(ext, lengthOf(ext), src.c_str());
		const u32 hash = crc32(ext);
		IPlugin* plugin;
		{
			MT::SpinLock lock(m_plugin_mutex);
			auto iter = m_plugins.find(hash);
			if (!iter.isValid()) {
				g_log_error.log("Editor") << "Asset compiler does not know how to compile " << src;
				return false;
			}
			plugin = iter.value();
		}
		return plugin->compile(src);
	}


	bool onBeforeLoad(Resource& res)
	{
		if (!PlatformInterface::fileExists(res.getPath().c_str())) return false;
		const u32 hash = res.getPath().getHash();
		const StaticString<MAX_PATH_LENGTH> dst_path(".lumix/assets/", hash, ".res");
		const PathUtils::FileInfo info(res.getPath().c_str());
		const StaticString<MAX_PATH_LENGTH> meta_path(info.m_dir, info.m_basename, ".meta");

		if (!PlatformInterface::fileExists(dst_path)
			|| PlatformInterface::getLastModified(dst_path) < PlatformInterface::getLastModified(res.getPath().c_str())
			|| PlatformInterface::getLastModified(dst_path) < PlatformInterface::getLastModified(meta_path)
			)
		{
			MT::SpinLock lock(m_to_compile_mutex);
			CompileEntry& e = m_to_compile.emplace();
			e.resource = &res;
			m_semaphore.signal();
			return true;
		}
		return false;
	}

	CompileEntry popCompiledResource()
	{
		MT::SpinLock lock(m_compiled_mutex);
		if(m_compiled.empty()) return CompileEntry{};
		CompileEntry e = m_compiled.back();
		m_compiled.pop();
		return e;
	}
	
	void update() override
	{
		for(;;) {
			CompileEntry e = popCompiledResource();
			if(e.resource) {
				m_load_hook.continueLoad(*e.resource);
			}
			else if (e.path.isValid()) {
				m_app.getWorldEditor().getEngine().getResourceManager().reload(e.path);
			}
			else {
				break;
			}
		}
	}


	void removePlugin(IPlugin& plugin) override
	{
		// TODO
		// do not forget that the task can be using the plugin, needs mutex
		ASSERT(false);
	}

	const char* getCompiledDir() const override { return ".lumix/assets/"; }


	void addPlugin(IPlugin& plugin, const char** extensions) override
	{
		const char** i = extensions;
		while(*i) {
			const u32 hash = crc32(*i);
			MT::SpinLock lock(m_plugin_mutex);
			m_plugins.insert(hash, &plugin);
			++i;
		}
	}

	MT::Semaphore m_semaphore;
	MT::SpinMutex m_to_compile_mutex;
	MT::SpinMutex m_compiled_mutex;
	MT::SpinMutex m_plugin_mutex;
	Array<CompileEntry> m_to_compile;
	Array<CompileEntry> m_compiled;
	StudioApp& m_app;
	LoadHook m_load_hook;
	HashMap<u32, IPlugin*> m_plugins;
	AssetCompilerTask m_task;
	FileSystemWatcher* m_watcher;
};


int AssetCompilerTask::task()
{
	while (!m_finished) {
		m_compiler.m_semaphore.wait();
		const AssetCompilerImpl::CompileEntry e = [&]{
			MT::SpinLock lock(m_compiler.m_to_compile_mutex);
			AssetCompilerImpl::CompileEntry e = m_compiler.m_to_compile.back();
			m_compiler.m_to_compile.pop();
			return e;
		}();
		if (e.resource) {
			m_compiler.compile(e.resource->getPath());
			MT::SpinLock lock(m_compiler.m_compiled_mutex);
			m_compiler.m_compiled.push(e);
		}
		else if(e.path.isValid()) {
			m_compiler.compile(e.path);
			MT::SpinLock lock(m_compiler.m_compiled_mutex);
			m_compiler.m_compiled.push(e);
		}
	}
	return 0;
}


AssetCompiler* AssetCompiler::create(StudioApp& app)
{
	return LUMIX_NEW(app.getWorldEditor().getAllocator(), AssetCompilerImpl)(app);
}


void AssetCompiler::destroy(AssetCompiler& compiler)
{
	AssetCompilerImpl& impl = (AssetCompilerImpl&)compiler;
	IAllocator& allocator = impl.m_app.getWorldEditor().getAllocator();
	LUMIX_DELETE(allocator, &compiler);
}


} // namespace Lumix
