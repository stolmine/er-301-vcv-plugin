// macOS implementation of od::enumerateModules() for the VCV plugin.
//
// The firmware ships two implementations of this (see hal/modulemap.h):
// am335x walks its custom dlopen registry, emu/linux uses dl_iterate_phdr.
// The VCV plugin matches the linux shape -- DSP packages are ordinary .so
// files opened by Lua's require through the system dynamic linker -- but
// macOS has no dl_iterate_phdr, so we walk dyld's image list instead.
//
// Without this, od/glue/CrashDiag.cpp fails to link: it calls
// od::enumerateModules() with no weak default, and neither arch that
// implements it is compiled into the plugin.

#include <hal/modulemap.h>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <string.h>

namespace od
{
  namespace
  {
    // Sum the vmsize of every __TEXT / __DATA* segment in a loaded Mach-O
    // image. vmaddr is the link-time address; the caller adds the slide to
    // get the runtime base. Returns false if the segment isn't present.
    bool segmentExtent(const struct mach_header *mh, const char *segName,
                       uintptr_t *vmaddr, size_t *vmsize)
    {
      // The plugin is 64-bit only; a 32-bit header means something is very
      // wrong, and its load commands have a different layout.
      if (mh->magic != MH_MAGIC_64)
        return false;

      const struct mach_header_64 *mh64 = (const struct mach_header_64 *)mh;
      const struct load_command *lc =
          (const struct load_command *)((const uint8_t *)mh64 + sizeof(*mh64));

      for (uint32_t i = 0; i < mh64->ncmds; i++)
      {
        if (lc->cmd == LC_SEGMENT_64)
        {
          const struct segment_command_64 *seg =
              (const struct segment_command_64 *)lc;
          if (strncmp(seg->segname, segName, sizeof(seg->segname)) == 0)
          {
            *vmaddr = (uintptr_t)seg->vmaddr;
            *vmsize = (size_t)seg->vmsize;
            return true;
          }
        }
        lc = (const struct load_command *)((const uint8_t *)lc + lc->cmdsize);
      }
      return false;
    }

    bool endsWith(const std::string &s, const char *suffix)
    {
      size_t n = strlen(suffix);
      return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }
  }

  void enumerateModules(std::vector<ModuleInfo> &out)
  {
    out.clear();

    // Identify our own dylib -- the engine lives there, so it plays the role
    // the "kernel" entry does on hardware. dladdr on a local symbol is the
    // only reliable way to get its path (same trick the plugin already uses
    // to re-dlopen itself RTLD_GLOBAL).
    std::string selfPath;
    Dl_info info;
    if (dladdr((const void *)&enumerateModules, &info) && info.dli_fname)
      selfPath = info.dli_fname;

    // Reserve slot 0 so the kernel entry stays first even though dyld may
    // report our image at any index.
    out.push_back(ModuleInfo{"kernel", 0, 0, 0, 0});

    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++)
    {
      const struct mach_header *mh = _dyld_get_image_header(i);
      const char *name = _dyld_get_image_name(i);
      if (!mh || !name)
        continue;

      std::string path = name;
      bool isSelf = !selfPath.empty() && path == selfPath;

      // Everything else in the image list is a system dylib or a Rack
      // library -- noise in a crash report. DSP packages are .so files.
      if (!isSelf && !endsWith(path, ".so"))
        continue;

      intptr_t slide = _dyld_get_image_vmaddr_slide(i);

      ModuleInfo m;
      m.path = isSelf ? "kernel" : path;
      m.textBase = 0;
      m.textSize = 0;
      m.dataBase = 0;
      m.dataSize = 0;

      uintptr_t addr = 0;
      size_t size = 0;
      if (segmentExtent(mh, SEG_TEXT, &addr, &size))
      {
        m.textBase = addr + (uintptr_t)slide;
        m.textSize = size;
      }
      // Prefer __DATA, but arm64 puts most writable data in __DATA_CONST;
      // report whichever is present so the offline symbolizer has a base.
      if (segmentExtent(mh, SEG_DATA, &addr, &size) ||
          segmentExtent(mh, "__DATA_CONST", &addr, &size))
      {
        m.dataBase = addr + (uintptr_t)slide;
        m.dataSize = size;
      }

      if (isSelf)
        out[0] = m;
      else
        out.push_back(m);
    }
  }
}
