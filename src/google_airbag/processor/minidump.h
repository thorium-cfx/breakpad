// Copyright (c) 2006, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// minidump.h: A minidump reader.
//
// The basic structure of this module tracks the structure of the minidump
// file itself.  At the top level, a minidump file is represented by a
// Minidump object.  Like most other classes in this module, Minidump
// provides a Read method that initializes the object with information from
// the file.  Most of the classes in this file are wrappers around the
// "raw" structures found in the minidump file itself, and defined in
// minidump_format.h.  For example, each thread is represented by a
// MinidumpThread object, whose parameters are specified in an MDRawThread
// structure.  A properly byte-swapped MDRawThread can be obtained from a
// MinidumpThread easily by calling its thread() method.
//
// Most of the module lazily reads only the portion of the minidump file
// necessary to fulfill the user's request.  Calling Minidump::Read
// only reads the minidump's directory.  The thread list is not read until
// it is needed, and even once it's read, the memory regions for each
// thread's stack aren't read until they're needed.  This strategy avoids
// unnecessary file input, and allocating memory for data in which the user
// has no interest.  Note that although memory allocations for a typical
// minidump file are not particularly large, it is possible for legitimate
// minidumps to be sizable.  A full-memory minidump, for example, contains
// a snapshot of the entire mapped memory space.  Even a normal minidump,
// with stack memory only, can be large if, for example, the dump was
// generated in response to a crash that occurred due to an infinite-
// recursion bug that caused the stack's limits to be exceeded.  Finally,
// some users of this library will unfortunately find themselves in the
// position of having to process potentially-hostile minidumps that might
// attempt to cause problems by forcing the minidump processor to over-
// allocate memory.
//
// Memory management in this module is based on a strict
// you-don't-own-anything policy.  The only object owned by the user is
// the top-level Minidump object, the creation and destruction of which
// must be the user's own responsibility.  All other objects obtained
// through interaction with this module are ultimately owned by the
// Minidump object, and will be freed upon the Minidump object's destruction.
// Because memory regions can potentially involve large allocations, a
// FreeMemory method is provided by MinidumpMemoryRegion, allowing the user
// to release data when it is no longer needed.  Use of this method is
// optional but recommended.  If freed data is later required, it will
// be read back in from the minidump file again.
//
// There is one exception to this memory management policy:
// Minidump::ReadString will return a string object to the user, and the user
// is responsible for its deletion.
//
// Author: Mark Mentovai

#ifndef GOOGLE_AIRBAG_PROCESSOR_MINIDUMP_H__
#define GOOGLE_AIRBAG_PROCESSOR_MINIDUMP_H__


#include <map>
#include <string>
#include <vector>

#include "google_airbag/common/minidump_format.h"
#include "google_airbag/processor/memory_region.h"


namespace google_airbag {


using std::map;
using std::string;
using std::vector;


class Minidump;
template<typename AddressType, typename EntryType> class RangeMap;


// MinidumpObject is the base of all Minidump* objects except for Minidump
// itself.
class MinidumpObject {
 public:
  virtual ~MinidumpObject() {}

 protected:
  explicit MinidumpObject(Minidump* minidump);

  // Refers to the Minidump object that is the ultimate parent of this
  // Some MinidumpObjects are owned by other MinidumpObjects, but at the
  // root of the ownership tree is always a Minidump.  The Minidump object
  // is kept here for access to its seeking and reading facilities, and
  // for access to data about the minidump file itself, such as whether
  // it should be byte-swapped.
  Minidump* minidump_;

  // MinidumpObjects are not valid when created.  When a subclass populates
  // its own fields, it can set valid_ to true.  Accessors and mutators may
  // wish to consider or alter the valid_ state as they interact with
  // objects.
  bool      valid_;
};


// This class exists primarily to provide a virtual destructor in a base
// class common to all objects that might be stored in
// Minidump::mStreamObjects.  Some object types (MinidumpContext) will
// never be stored in Minidump::mStreamObjects, but are represented as
// streams and adhere to the same interface, and may be derived from
// this class.
class MinidumpStream : public MinidumpObject {
 public:
  virtual ~MinidumpStream() {}

 protected:
  explicit MinidumpStream(Minidump* minidump);

 private:
  // Populate (and validate) the MinidumpStream.  minidump_ is expected
  // to be positioned at the beginning of the stream, so that the next
  // read from the minidump will be at the beginning of the stream.
  // expected_size should be set to the stream's length as contained in
  // the MDRawDirectory record or other identifying record.  A class
  // that implements MinidumpStream can compare expected_size to a
  // known size as an integrity check.
  virtual bool Read(u_int32_t expected_size) = 0;
};


// MinidumpContext carries a CPU-specific MDRawContext structure, which
// contains CPU context such as register states.  Each thread has its
// own context, and the exception record, if present, also has its own
// context.  Note that if the exception record is present, the context it
// refers to is probably what the user wants to use for the exception
// thread, instead of that thread's own context.  The exception thread's
// context (as opposed to the exception record's context) will contain
// context for the exception handler (which performs minidump generation),
// and not the context that caused the exception (which is probably what the
// user wants).
class MinidumpContext : public MinidumpStream {
 public:
  ~MinidumpContext();

  // Returns an MD_CONTEXT_* value such as MD_CONTEXT_X86 or MD_CONTEXT_PPC
  // identifying the CPU type that the context was collected from.  The
  // returned value will identify the CPU only, and will have any other
  // MD_CONTEXT_* bits masked out.  Returns 0 on failure.
  u_int32_t GetContextCPU() const;

  // Returns raw CPU-specific context data for the named CPU type.  If the
  // context data does not match the CPU type or does not exist, returns
  // NULL.
  const MDRawContextX86* GetContextX86() const;
  const MDRawContextPPC* GetContextPPC() const;

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  friend class MinidumpThread;
  friend class MinidumpException;

  explicit MinidumpContext(Minidump* minidump);

  bool Read(u_int32_t expected_size);

  // Free the CPU-specific context structure.
  void FreeContext();

  // If the minidump contains a SYSTEM_INFO_STREAM, makes sure that the
  // system info stream gives an appropriate CPU type matching the context
  // CPU type in context_cpu_type.  Returns false if the CPU type does not
  // match.  Returns true if the CPU type matches or if the minidump does
  // not contain a system info stream.
  bool CheckAgainstSystemInfo(u_int32_t context_cpu_type);

  // The CPU-specific context structure.
  union {
    MDRawContextBase* base;
    MDRawContextX86*  x86;
    MDRawContextPPC*  ppc;
  } context_;
};


// MinidumpMemoryRegion does not wrap any MDRaw structure, and only contains
// a reference to an MDMemoryDescriptor.  This object is intended to wrap
// portions of a minidump file that contain memory dumps.  In normal
// minidumps, each MinidumpThread owns a MinidumpMemoryRegion corresponding
// to the thread's stack memory.  MinidumpMemoryList also gives access to
// memory regions in its list as MinidumpMemoryRegions.  This class
// adheres to MemoryRegion so that it may be used as a data provider to
// the Stackwalker family of classes.
class MinidumpMemoryRegion : public MinidumpObject,
                             public MemoryRegion {
 public:
  ~MinidumpMemoryRegion();

  // Returns a pointer to the base of the memory region.  Returns the
  // cached value if available, otherwise, reads the minidump file and
  // caches the memory region.
  const u_int8_t* GetMemory();

  // The address of the base of the memory region.
  u_int64_t GetBase();

  // The size, in bytes, of the memory region.
  u_int32_t GetSize();

  // Frees the cached memory region, if cached.
  void FreeMemory();

  // Obtains the value of memory at the pointer specified by address.
  bool GetMemoryAtAddress(u_int64_t address, u_int8_t*  value);
  bool GetMemoryAtAddress(u_int64_t address, u_int16_t* value);
  bool GetMemoryAtAddress(u_int64_t address, u_int32_t* value);
  bool GetMemoryAtAddress(u_int64_t address, u_int64_t* value);

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  friend class MinidumpThread;
  friend class MinidumpMemoryList;

  explicit MinidumpMemoryRegion(Minidump* minidump);

  // Identify the base address and size of the memory region, and the
  // location it may be found in the minidump file.
  void SetDescriptor(MDMemoryDescriptor* descriptor);

  // Implementation for GetMemoryAtAddress
  template<typename T> bool GetMemoryAtAddressInternal(u_int64_t address,
                                                       T*        value);

  // Base address and size of the memory region, and its position in the
  // minidump file.
  MDMemoryDescriptor* descriptor_;

  // Cached memory.
  vector<u_int8_t>*   memory_;
};


// MinidumpThread contains information about a thread of execution,
// including a snapshot of the thread's stack and CPU context.  For
// the thread that caused an exception, the context carried by
// MinidumpException is probably desired instead of the CPU context
// provided here.
class MinidumpThread : public MinidumpObject {
 public:
  ~MinidumpThread();

  const MDRawThread* thread() const { return valid_ ? &thread_ : NULL; }
  MinidumpMemoryRegion* GetMemory();
  MinidumpContext* GetContext();

  // The thread ID is used to determine if a thread is the exception thread,
  // so a special getter is provided to retrieve this data from the
  // MDRawThread structure.  Returns false if the thread ID cannot be
  // determined.
  bool GetThreadID(u_int32_t *thread_id) const;

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  // These objects are managed by MinidumpThreadList.
  friend class MinidumpThreadList;

  explicit MinidumpThread(Minidump* minidump);

  // This works like MinidumpStream::Read, but is driven by
  // MinidumpThreadList.  No size checking is done, because
  // MinidumpThreadList handles that directly.
  bool Read();

  MDRawThread           thread_;
  MinidumpMemoryRegion* memory_;
  MinidumpContext*      context_;
};


// MinidumpThreadList contains all of the threads (as MinidumpThreads) in
// a process.
class MinidumpThreadList : public MinidumpStream {
 public:
  ~MinidumpThreadList();

  unsigned int thread_count() const { return valid_ ? thread_count_ : 0; }

  // Sequential access to threads.
  MinidumpThread* GetThreadAtIndex(unsigned int index) const;

  // Random access to threads.
  MinidumpThread* GetThreadByID(u_int32_t thread_id);

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  friend class Minidump;

  typedef map<u_int32_t, MinidumpThread*> IDToThreadMap;
  typedef vector<MinidumpThread> MinidumpThreads;

  static const u_int32_t kStreamType = MD_THREAD_LIST_STREAM;

  explicit MinidumpThreadList(Minidump* aMinidump);

  bool Read(u_int32_t aExpectedSize);

  // Access to threads using the thread ID as the key.
  IDToThreadMap    id_to_thread_map_;

  // The list of threads.
  MinidumpThreads* threads_;
  u_int32_t        thread_count_;
};


// MinidumpModule wraps MDRawModule, which contains information about loaded
// code modules.  Access is provided to various data referenced indirectly
// by MDRawModule, such as the module's name and a specification for where
// to locate debugging information for the module.
class MinidumpModule : public MinidumpObject {
 public:
  ~MinidumpModule();

  const MDRawModule* module() const { return valid_ ? &module_ : NULL; }
  u_int64_t base_address() const {
    return valid_ ? module_.base_of_image : static_cast<u_int64_t>(-1);
  }
  u_int32_t size() const { return valid_ ? module_.size_of_image : 0; }

  // The name of the file containing this module's code (exe, dll, so,
  // dylib).
  const string* GetName();

  // The CodeView record, which contains information to locate the module's
  // debugging information (pdb).  This is returned as u_int8_t* because
  // the data can be one of two types: MDCVInfoPDB20* or MDCVInfoPDB70*.
  // Check the record's signature in the first four bytes to differentiate.
  // Current toolchains generate modules which carry MDCVInfoPDB70.
  const u_int8_t* GetCVRecord();

  // The miscellaneous debug record, which is obsolete.  Current toolchains
  // do not generate this type of debugging information (dbg), and this
  // field is not expected to be present.
  const MDImageDebugMisc* GetMiscRecord();

  // The filename of the file containing debugging information for this
  // module.  This data is supplied by the CodeView record, if present, or
  // the miscellaneous debug record.  As such, it will reference either a
  // pdb or dbg file.
  const string* GetDebugFilename();

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  // These objects are managed by MinidumpModuleList.
  friend class MinidumpModuleList;

  explicit MinidumpModule(Minidump* minidump);

  // This works like MinidumpStream::Read, but is driven by
  // MinidumpModuleList.  No size checking is done, because
  // MinidumpModuleList handles that directly.
  bool Read();

  MDRawModule       module_;

  // Cached module name.
  const string*     name_;

  // Cached CodeView record - this is MDCVInfoPDB20 or (likely)
  // MDCVInfoPDB70.  Stored as a u_int8_t because the structure contains
  // a variable-sized string and its exact size cannot be known until it
  // is processed.
  vector<u_int8_t>* cv_record_;

  // Cached MDImageDebugMisc (usually not present), stored as u_int8_t
  // because the structure contains a variable-sized string and its exact
  // size cannot be known until it is processed.
  vector<u_int8_t>* misc_record_;

  // Cached debug filename.
  const string*     debug_filename_;
};


// MinidumpModuleList contains all of the loaded code modules for a process
// in the form of MinidumpModules.  It maintains a map of these modules
// so that it may easily provide a code module corresponding to a specific
// address.
class MinidumpModuleList : public MinidumpStream {
 public:
  ~MinidumpModuleList();

  unsigned int module_count() const { return valid_ ? module_count_ : 0; }

  // Sequential access to modules.
  MinidumpModule* GetModuleAtIndex(unsigned int index) const;

  // Random access to modules.  Returns the module whose code is present
  // at the address identified by address.
  MinidumpModule* GetModuleForAddress(u_int64_t address);

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  friend class Minidump;

  typedef vector<MinidumpModule> MinidumpModules;

  static const u_int32_t kStreamType = MD_MODULE_LIST_STREAM;

  explicit MinidumpModuleList(Minidump* minidump);

  bool Read(u_int32_t expected_size);

  // Access to modules using addresses as the key.
  RangeMap<u_int64_t, unsigned int> *range_map_;

  MinidumpModules *modules_;
  u_int32_t module_count_;
};


// MinidumpMemoryList corresponds to a minidump's MEMORY_LIST_STREAM stream,
// which references the snapshots of all of the memory regions contained
// within the minidump.  For a normal minidump, this includes stack memory
// (also referenced by each MinidumpThread, in fact, the MDMemoryDescriptors
// here and in MDRawThread both point to exactly the same data in a
// minidump file, conserving space), as well as a 256-byte snapshot of memory
// surrounding the instruction pointer in the case of an exception.  Other
// types of minidumps may contain significantly more memory regions.  Full-
// memory minidumps contain all of a process' mapped memory.
class MinidumpMemoryList : public MinidumpStream {
 public:
  ~MinidumpMemoryList();

  unsigned int region_count() const { return valid_ ? region_count_ : 0; }

  // Sequential access to memory regions.
  MinidumpMemoryRegion* GetMemoryRegionAtIndex(unsigned int index);

  // Random access to memory regions.  Returns the region encompassing
  // the address identified by address.
  MinidumpMemoryRegion* GetMemoryRegionForAddress(u_int64_t address);

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  friend class Minidump;

  typedef vector<MDMemoryDescriptor>   MemoryDescriptors;
  typedef vector<MinidumpMemoryRegion> MemoryRegions;

  static const u_int32_t kStreamType = MD_MEMORY_LIST_STREAM;

  explicit MinidumpMemoryList(Minidump* minidump);

  bool Read(u_int32_t expected_size);

  // Access to memory regions using addresses as the key.
  RangeMap<u_int64_t, unsigned int> *range_map_;

  // The list of descriptors.  This is maintained separately from the list
  // of regions, because MemoryRegion doesn't own its MemoryDescriptor, it
  // maintains a pointer to it.  descriptors_ provides the storage for this
  // purpose.
  MemoryDescriptors *descriptors_;

  // The list of regions.
  MemoryRegions *regions_;
  u_int32_t region_count_;
};


// MinidumpException wraps MDRawExceptionStream, which contains information
// about the exception that caused the minidump to be generated, if the
// minidump was generated in an exception handler called as a result of
// an exception.  It also provides access to a MinidumpContext object,
// which contains the CPU context for the exception thread at the time
// the exception occurred.
class MinidumpException : public MinidumpStream {
 public:
  ~MinidumpException();

  const MDRawExceptionStream* exception() const {
    return valid_ ? &exception_ : NULL;
  }

  // The thread ID is used to determine if a thread is the exception thread,
  // so a special getter is provided to retrieve this data from the
  // MDRawExceptionStream structure.  Returns false if the thread ID cannot
  // be determined.
  bool GetThreadID(u_int32_t *thread_id) const;

  MinidumpContext* GetContext();

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  friend class Minidump;

  static const u_int32_t kStreamType = MD_EXCEPTION_STREAM;

  explicit MinidumpException(Minidump* minidump);

  bool Read(u_int32_t expected_size);

  MDRawExceptionStream exception_;
  MinidumpContext*     context_;
};


// MinidumpSystemInfo wraps MDRawSystemInfo and provides information about
// the system on which the minidump was generated.  See also MinidumpMiscInfo.
class MinidumpSystemInfo : public MinidumpStream {
 public:
  ~MinidumpSystemInfo();

  const MDRawSystemInfo* system_info() const {
    return valid_ ? &system_info_ : NULL;
  }

  // I don't know what CSD stands for, but this field is documented as
  // returning a textual representation of the OS service pack.  On other
  // platforms, this provides additional information about an OS version
  // level beyond major.minor.micro.  Returns NULL if unknown.
  const string* GetCSDVersion();

  // If a CPU vendor string can be determined, returns a pointer to it,
  // otherwise, returns NULL.  CPU vendor strings can be determined from
  // x86 CPUs with CPUID 0.
  const string* GetCPUVendor();

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  friend class Minidump;

  static const u_int32_t kStreamType = MD_SYSTEM_INFO_STREAM;

  explicit MinidumpSystemInfo(Minidump* minidump);

  bool Read(u_int32_t expected_size);

  MDRawSystemInfo system_info_;

  // Textual representation of the OS service pack, for minidumps produced
  // by MiniDumpWriteDump on Windows.
  const string* csd_version_;

  // A string identifying the CPU vendor, if known.
  const string* cpu_vendor_;
};


// MinidumpMiscInfo wraps MDRawMiscInfo and provides information about
// the process that generated the minidump, and optionally additional system
// information.  See also MinidumpSystemInfo.
class MinidumpMiscInfo : public MinidumpStream {
 public:
  const MDRawMiscInfo* misc_info() const {
    return valid_ ? &misc_info_ : NULL;
  }

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  friend class Minidump;

  static const u_int32_t kStreamType = MD_MISC_INFO_STREAM;

  explicit MinidumpMiscInfo(Minidump* minidump_);

  bool Read(u_int32_t expected_size_);

  MDRawMiscInfo misc_info_;
};


// MinidumpAirbagInfo wraps MDRawAirbagInfo, which is an optional stream in
// a minidump that provides additional information about the process state
// at the time the minidump was generated.
class MinidumpAirbagInfo : public MinidumpStream {
 public:
  const MDRawAirbagInfo* airbag_info() const {
    return valid_ ? &airbag_info_ : NULL;
  }

  // These thread IDs are used to determine if threads deserve special
  // treatment, so special getters are provided to retrieve this data from
  // the MDRawAirbagInfo structure.  The getters return false if the thread
  // IDs cannot be determined.
  bool GetDumpThreadID(u_int32_t *thread_id) const;
  bool GetRequestingThreadID(u_int32_t *thread_id) const;

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  friend class Minidump;

  static const u_int32_t kStreamType = MD_AIRBAG_INFO_STREAM;

  explicit MinidumpAirbagInfo(Minidump* minidump_);

  bool Read(u_int32_t expected_size_);

  MDRawAirbagInfo airbag_info_;
};


// Minidump is the user's interface to a minidump file.  It wraps MDRawHeader
// and provides access to the minidump's top-level stream directory.
class Minidump {
 public:
  // path is the pathname of a file containing the minidump.
  explicit Minidump(const string& path);

  ~Minidump();

  const MDRawHeader* header() const { return valid_ ? &header_ : NULL; }

  // Reads the minidump file's header and top-level stream directory.
  // The minidump is expected to be positioned at the beginning of the
  // header.  Read() sets up the stream list and map, and validates the
  // Minidump object.
  bool Read();

  // The next set of methods are stubs that call GetStream.  They exist to
  // force code generation of the templatized API within the module, and
  // to avoid exposing an ugly API (GetStream needs to accept a garbage
  // parameter).
  MinidumpThreadList* GetThreadList();
  MinidumpModuleList* GetModuleList();
  MinidumpMemoryList* GetMemoryList();
  MinidumpException* GetException();
  MinidumpSystemInfo* GetSystemInfo();
  MinidumpMiscInfo* GetMiscInfo();
  MinidumpAirbagInfo* GetAirbagInfo();

  // The next set of methods are provided for users who wish to access
  // data in minidump files directly, while leveraging the rest of
  // this class and related classes to handle the basic minidump
  // structure and known stream types.

  unsigned int GetDirectoryEntryCount() const {
    return valid_ ? header_.stream_count : 0;
  }
  const MDRawDirectory* GetDirectoryEntryAtIndex(unsigned int index) const;

  // The next 2 methods are lower-level I/O routines.  They use fd_.

  // Reads count bytes from the minidump at the current position into
  // the storage area pointed to by bytes.  bytes must be of sufficient
  // size.  After the read, the file position is advanced by count.
  bool ReadBytes(void* bytes, size_t count);

  // Sets the position of the minidump file to offset.
  bool SeekSet(off_t offset);

  // The next 2 methods are medium-level I/O routines.

  // ReadString returns a string which is owned by the caller!  offset
  // specifies the offset that a length-encoded string is stored at in the
  // minidump file.
  string* ReadString(off_t offset);

  // SeekToStreamType positions the file at the beginning of a stream
  // identified by stream_type, and informs the caller of the stream's
  // length by setting *stream_length.  Because stream_map maps each stream
  // type to only one stream in the file, this might mislead the user into
  // thinking that the stream that this seeks to is the only stream with
  // type stream_type.  That can't happen for streams that these classes
  // deal with directly, because they're only supposed to be present in the
  // file singly, and that's verified when stream_map_ is built.  Users who
  // are looking for other stream types should be aware of this
  // possibility, and consider using GetDirectoryEntryAtIndex (possibly
  // with GetDirectoryEntryCount) if expecting multiple streams of the same
  // type in a single minidump file.
  bool SeekToStreamType(u_int32_t stream_type, u_int32_t* stream_length);

  bool swap() const { return valid_ ? swap_ : false; }

  // Print a human-readable representation of the object to stdout.
  void Print();

 private:
  // MinidumpStreamInfo is used in the MinidumpStreamMap.  It lets
  // the Minidump object locate interesting streams quickly, and
  // provides a convenient place to stash MinidumpStream objects.
  struct MinidumpStreamInfo {
    MinidumpStreamInfo() : stream_index(0), stream(NULL) {}
    ~MinidumpStreamInfo() { delete stream; }

    // Index into the MinidumpDirectoryEntries vector
    unsigned int    stream_index;

    // Pointer to the stream if cached, or NULL if not yet populated
    MinidumpStream* stream;
  };

  typedef vector<MDRawDirectory> MinidumpDirectoryEntries;
  typedef map<u_int32_t, MinidumpStreamInfo> MinidumpStreamMap;

  template<typename T> T* GetStream(T** stream);

  // Opens the minidump file, or if already open, seeks to the beginning.
  bool Open();

  MDRawHeader               header_;

  // The list of streams.
  MinidumpDirectoryEntries* directory_;

  // Access to streams using the stream type as the key.
  MinidumpStreamMap*        stream_map_;

  // The pathname of the minidump file to process, set in the constructor.
  const string              path_;

  // The file descriptor for all file I/O.  Used by ReadBytes and SeekSet.
  // Set based on the |path_| member by Open, which is called by Read.
  int                       fd_;

  // swap_ is true if the minidump file should be byte-swapped.  If the
  // minidump was produced by a CPU that is other-endian than the CPU
  // processing the minidump, this will be true.  If the two CPUs are
  // same-endian, this will be false.
  bool                      swap_;

  // Validity of the Minidump structure, false immediately after
  // construction or after a failed Read(); true following a successful
  // Read().
  bool                      valid_;
};


}  // namespace google_airbag


#endif  // GOOGLE_AIRBAG_PROCESSOR_MINIDUMP_H__