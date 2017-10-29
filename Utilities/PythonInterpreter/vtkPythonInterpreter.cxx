/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkPythonInterpreter.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkPython.h" // this must be the first include.
#include "vtkPythonInterpreter.h"

#include "vtkCommand.h"
#include "vtkObjectFactory.h"
#include "vtkPythonStdStreamCaptureHelper.h"
#include "vtkVersion.h"
#include "vtkWeakPointer.h"

#include <vtksys/SystemInformation.hxx>
#include <vtksys/SystemTools.hxx>

#include <algorithm>
#include <signal.h>
#include <sstream>
#include <string>
#include <vector>


#if defined(_WIN32) && !defined(__CYGWIN__)
// Implementation for Windows win32 code but not cygwin
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if PY_VERSION_HEX >= 0x03000000
#if defined(__APPLE__) && PY_VERSION_HEX < 0x03050000
extern "C" {
extern wchar_t* _Py_DecodeUTF8_surrogateescape(const char* s, Py_ssize_t size);
}
#endif
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
#define VTK_PATH_SEPARATOR "\\"
#else
#define VTK_PATH_SEPARATOR "/"
#endif

#define VTKPY_DEBUG_MESSAGE(x)                                                                     \
  if (vtkPythonInterpreter::GetPythonVerboseFlag() > 0)                                            \
  {                                                                                                \
    cout << "# vtk: " x << endl;                                                                   \
  }

#define VTKPY_DEBUG_MESSAGE_VV(x)                                                                  \
  if (vtkPythonInterpreter::GetPythonVerboseFlag() > 1)                                            \
  {                                                                                                \
    cout << "# vtk: " x << endl;                                                                   \
  }

namespace
{

template <class T>
void strFree(T* foo)
{
  delete[] foo;
}

template <class T>
class PoolT
{
  std::vector<T*> Strings;

public:
  ~PoolT()
  {
    for (T* astring : this->Strings)
    {
      strFree(astring);
    }
  }

  T* push_back(T* val)
  {
    this->Strings.push_back(val);
    return val;
  }
};

using StringPool = PoolT<char>;
#if PY_VERSION_HEX >= 0x03000000
template <>
void strFree(wchar_t* foo)
{
#if PY_VERSION_HEX >= 0x03050000
  PyMem_RawFree(foo);
#else
  PyMem_Free(foo);
#endif
}
using WCharStringPool = PoolT<wchar_t>;
#endif

#if PY_VERSION_HEX >= 0x03000000
wchar_t* vtk_Py_DecodeLocale(const char* arg, size_t* size)
{
  (void)size;
#if PY_VERSION_HEX >= 0x03050000
  return Py_DecodeLocale(arg, size);
#elif defined(__APPLE__)
  return _Py_DecodeUTF8_surrogateescape(arg, strlen(arg));
#else
  return _Py_char2wchar(arg, size);
#endif
}
#endif

#if PY_VERSION_HEX >= 0x03000000
char* vtk_Py_EncodeLocale(const wchar_t* arg, size_t* size)
{
  (void)size;
#if PY_VERSION_HEX >= 0x03050000
  return Py_EncodeLocale(arg, size);
#else
  return _Py_wchar2char(arg, size);
#endif
}
#endif

std::string GetLibraryForSymbol(const char* symbolname)
{
#if defined(_WIN32) && !defined(__CYGWIN__)
  (void)symbolname;
  // Use the path to the running exe as the start of a search prefix
  HMODULE handle = GetModuleHandle(NULL);
  if (!handle)
  { // Can't find ourselves????? this shouldn't happen
    return std::string();
  }

  TCHAR path[MAX_PATH];
  if (!GetModuleFileName(handle, path, MAX_PATH))
  {
    return std::string();
  }
  return std::string(path);
#else // *NIX and macOS
  // Use the library location of VTK's python wrapping as a prefix to search
  void* handle = dlsym(RTLD_NEXT, symbolname);
  if (!handle)
  {
    return std::string();
  }

  Dl_info di;
  int ret = dladdr(handle, &di);
  if (ret == 0 || !di.dli_saddr || !di.dli_fname)
  {
    return std::string();
  }
  return std::string(di.dli_fname);
#endif
}

static std::vector<vtkWeakPointer<vtkPythonInterpreter> > GlobalInterpreters;
static std::vector<std::string> PythonPaths;

void NotifyInterpreters(unsigned long eventid, void* calldata = nullptr)
{
  std::vector<vtkWeakPointer<vtkPythonInterpreter> >::iterator iter;
  for (iter = GlobalInterpreters.begin(); iter != GlobalInterpreters.end(); ++iter)
  {
    if (iter->GetPointer())
    {
      iter->GetPointer()->InvokeEvent(eventid, calldata);
    }
  }
}

inline void vtkPrependPythonPath(const char* pathtoadd)
{
  VTKPY_DEBUG_MESSAGE("adding module search path " << pathtoadd);
  vtkPythonScopeGilEnsurer gilEnsurer;
  PyObject* path = PySys_GetObject(const_cast<char*>("path"));
#if PY_VERSION_HEX >= 0x03000000
  PyObject* newpath = PyUnicode_FromString(pathtoadd);
#else
  PyObject* newpath = PyString_FromString(pathtoadd);
#endif
  PyList_Insert(path, 0, newpath);
  Py_DECREF(newpath);
}

inline void vtkSafePrependPythonPath(const std::string& pathtoadd)
{
  VTKPY_DEBUG_MESSAGE_VV("trying " << pathtoadd);
  if (!pathtoadd.empty() && vtksys::SystemTools::FileIsDirectory(pathtoadd))
  {
    vtkPrependPythonPath(pathtoadd.c_str());
  }
}
}

bool vtkPythonInterpreter::InitializedOnce = false;
bool vtkPythonInterpreter::CaptureStdin = false;
bool vtkPythonInterpreter::ConsoleBuffering = false;
std::string vtkPythonInterpreter::StdErrBuffer;
std::string vtkPythonInterpreter::StdOutBuffer;
int vtkPythonInterpreter::PythonVerboseFlag = 0;

vtkStandardNewMacro(vtkPythonInterpreter);
//----------------------------------------------------------------------------
vtkPythonInterpreter::vtkPythonInterpreter()
{
  GlobalInterpreters.push_back(this);
}

//----------------------------------------------------------------------------
vtkPythonInterpreter::~vtkPythonInterpreter()
{
  std::vector<vtkWeakPointer<vtkPythonInterpreter> >::iterator iter;
  for (iter = GlobalInterpreters.begin(); iter != GlobalInterpreters.end(); ++iter)
  {
    if (*iter == this)
    {
      GlobalInterpreters.erase(iter);
      break;
    }
  }
}

//----------------------------------------------------------------------------
bool vtkPythonInterpreter::IsInitialized()
{
  return (Py_IsInitialized() != 0);
}

//----------------------------------------------------------------------------
void vtkPythonInterpreter::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

//----------------------------------------------------------------------------
bool vtkPythonInterpreter::Initialize(int initsigs /*=0*/)
{
  if (Py_IsInitialized() == 0)
  {
    // guide the mechanism to locate Python standard library, if possible.
    vtkPythonInterpreter::SetupPythonPrefix();

    Py_InitializeEx(initsigs);

#ifdef SIGINT
    // Put default SIGINT handler back after Py_Initialize/Py_InitializeEx.
    signal(SIGINT, SIG_DFL);
#endif
  }

  if (!vtkPythonInterpreter::InitializedOnce)
  {
    vtkPythonInterpreter::InitializedOnce = true;

#ifdef VTK_PYTHON_FULL_THREADSAFE
    int threadInit = PyEval_ThreadsInitialized();
    PyEval_InitThreads(); // safe to call this multiple time
    if (!threadInit)
    {
      PyEval_SaveThread(); // release GIL
    }
#endif

    // HACK: Calling PyRun_SimpleString for the first time for some reason results in
    // a "\n" message being generated which is causing the error dialog to
    // popup. So we flush that message out of the system before setting up the
    // callbacks.
    vtkPythonInterpreter::RunSimpleString("");

    // Redirect Python's stdout and stderr and stdin - GIL protected operation
    {
      // Setup handlers for stdout/stdin/stderr.
      vtkPythonStdStreamCaptureHelper* wrapperOut = NewPythonStdStreamCaptureHelper(false);
      vtkPythonStdStreamCaptureHelper* wrapperErr = NewPythonStdStreamCaptureHelper(true);
      vtkPythonScopeGilEnsurer gilEnsurer;
      PySys_SetObject(const_cast<char*>("stdout"), reinterpret_cast<PyObject*>(wrapperOut));
      PySys_SetObject(const_cast<char*>("stderr"), reinterpret_cast<PyObject*>(wrapperErr));
      PySys_SetObject(const_cast<char*>("stdin"), reinterpret_cast<PyObject*>(wrapperOut));
      Py_DECREF(wrapperOut);
      Py_DECREF(wrapperErr);
    }

    vtkPythonInterpreter::SetupVTKPythonPaths();

    for (size_t cc = 0; cc < PythonPaths.size(); cc++)
    {
      vtkPrependPythonPath(PythonPaths[cc].c_str());
    }

    NotifyInterpreters(vtkCommand::EnterEvent);
    return true;
  }

  return false;
}

//----------------------------------------------------------------------------
void vtkPythonInterpreter::Finalize()
{
  if (Py_IsInitialized() != 0)
  {
    NotifyInterpreters(vtkCommand::ExitEvent);
    vtkPythonScopeGilEnsurer gilEnsurer(false, true);
    // Py_Finalize will take care of relasing gil
    Py_Finalize();
  }
}

//----------------------------------------------------------------------------
void vtkPythonInterpreter::SetProgramName(const char* programname)
{
  if (programname)
  {
// From Python Docs: The argument should point to a zero-terminated character
// string in static storage whose contents will not change for the duration of
// the program's execution. No code in the Python interpreter will change the
// contents of this storage.
#if PY_VERSION_HEX >= 0x03000000
    wchar_t* argv0 = vtk_Py_DecodeLocale(programname, nullptr);
    if (argv0 == 0)
    {
      fprintf(stderr, "Fatal vtkpython error: "
                      "unable to decode the program name\n");
      static wchar_t empty[1] = { 0 };
      argv0 = empty;
      Py_SetProgramName(argv0);
    }
    else
    {
      static WCharStringPool wpool;
      Py_SetProgramName(wpool.push_back(argv0));
    }
#else
    static StringPool pool;
    Py_SetProgramName(pool.push_back(vtksys::SystemTools::DuplicateString(programname)));
#endif
  }
}

//----------------------------------------------------------------------------
void vtkPythonInterpreter::PrependPythonPath(const char* dir)
{
  if (!dir)
  {
    return;
  }

  std::string out_dir = dir;

#if defined(_WIN32) && !defined(__CYGWIN__)
  // Convert slashes for this platform.
  std::replace(out_dir.begin(), out_dir.end(), '/', '\\');
#endif

  // save path for future use.
  PythonPaths.push_back(out_dir);

  if (Py_IsInitialized() == 0)
  {
    return;
  }

  // Append the path to the python sys.path object.
  vtkPrependPythonPath(out_dir.c_str());
}

//----------------------------------------------------------------------------
int vtkPythonInterpreter::PyMain(int argc, char** argv)
{
  vtksys::SystemTools::EnableMSVCDebugHook();
  vtkPythonInterpreter::PythonVerboseFlag = 0;
  for (int cc = 0; cc < argc; ++cc)
  {
    if (argv[cc] && strcmp(argv[cc], "-v") == 0)
    {
      vtkPythonInterpreter::PythonVerboseFlag += 1;
    }
    if (argv[cc] && strcmp(argv[cc], "-vv") == 0)
    {
      vtkPythonInterpreter::PythonVerboseFlag = 2;
    }
  }
  vtkPythonInterpreter::Initialize(1);

#if PY_VERSION_HEX >= 0x03000000
  // Need two copies of args, because programs might modify the first
  wchar_t** argvWide = new wchar_t*[argc];
  wchar_t** argvWide2 = new wchar_t*[argc];
  int argcWide = 0;
  for (int i = 0; i < argc; i++)
  {
    if (argv[i] && strcmp(argv[i], "--enable-bt") == 0)
    {
      vtksys::SystemInformation::SetStackTraceOnError(1);
      continue;
    }
    if (argv[i] && strcmp(argv[i], "-V") == 0)
    {
      // print out VTK version and let argument pass to Py_Main(). At which point,
      // Python will print its version and exit.
      cout << vtkVersion::GetVTKSourceVersion() << endl;
    }

    argvWide[argcWide] = vtk_Py_DecodeLocale(argv[i], nullptr);
    argvWide2[argcWide] = argvWide[argcWide];
    if (argvWide[argcWide] == 0)
    {
      fprintf(stderr, "Fatal vtkpython error: "
                      "unable to decode the command line argument #%i\n",
        i + 1);
      for (int k = 0; k < argcWide; k++)
      {
        PyMem_Free(argvWide2[k]);
      }
      delete[] argvWide;
      delete[] argvWide2;
      return 1;
    }
    argcWide++;
  }
  vtkPythonScopeGilEnsurer gilEnsurer;
  int res = Py_Main(argcWide, argvWide);
  for (int i = 0; i < argcWide; i++)
  {
    PyMem_Free(argvWide2[i]);
  }
  delete[] argvWide;
  delete[] argvWide2;
  return res;
#else

  // process command line argments to remove unhandled args.
  std::vector<char*> newargv;
  for (int i=0; i < argc; ++i)
  {
    if (argv[i] && strcmp(argv[i], "--enable-bt") == 0)
    {
      vtksys::SystemInformation::SetStackTraceOnError(1);
      continue;
    }
    if (argv[i] && strcmp(argv[i], "-V") == 0)
    {
      // print out VTK version and let argument pass to Py_Main(). At which point,
      // Python will print its version and exit.
      cout << vtkVersion::GetVTKSourceVersion() << endl;
    }
    newargv.push_back(argv[i]);
  }

  vtkPythonScopeGilEnsurer gilEnsurer(false, true);
  return Py_Main(static_cast<int>(newargv.size()), &newargv[0]);
#endif
}

//----------------------------------------------------------------------------
int vtkPythonInterpreter::RunSimpleString(const char* script)
{
  vtkPythonInterpreter::Initialize(1);
  vtkPythonInterpreter::ConsoleBuffering = true;

  // The embedded python interpreter cannot handle DOS line-endings, see
  // http://sourceforge.net/tracker/?group_id=5470&atid=105470&func=detail&aid=1167922
  std::string buffer = script ? script : "";
  buffer.erase(std::remove(buffer.begin(), buffer.end(), '\r'), buffer.end());

  // The cast is necessary because PyRun_SimpleString() hasn't always been const-correct
  int pyReturn;
  {
    vtkPythonScopeGilEnsurer gilEnsurer;
    pyReturn = PyRun_SimpleString(const_cast<char*>(buffer.c_str()));
  }

  vtkPythonInterpreter::ConsoleBuffering = false;
  if (!vtkPythonInterpreter::StdErrBuffer.empty())
  {
    vtkOutputWindowDisplayErrorText(vtkPythonInterpreter::StdErrBuffer.c_str());
    NotifyInterpreters(
      vtkCommand::ErrorEvent, const_cast<char*>(vtkPythonInterpreter::StdErrBuffer.c_str()));
    vtkPythonInterpreter::StdErrBuffer.clear();
  }
  if (!vtkPythonInterpreter::StdOutBuffer.empty())
  {
    vtkOutputWindowDisplayText(vtkPythonInterpreter::StdOutBuffer.c_str());
    NotifyInterpreters(
      vtkCommand::SetOutputEvent, const_cast<char*>(vtkPythonInterpreter::StdOutBuffer.c_str()));
    vtkPythonInterpreter::StdOutBuffer.clear();
  }

  return pyReturn;
}

//----------------------------------------------------------------------------
void vtkPythonInterpreter::SetCaptureStdin(bool val)
{
  vtkPythonInterpreter::CaptureStdin = val;
}

//----------------------------------------------------------------------------
bool vtkPythonInterpreter::GetCaptureStdin()
{
  return vtkPythonInterpreter::CaptureStdin;
}

//----------------------------------------------------------------------------
void vtkPythonInterpreter::WriteStdOut(const char* txt)
{
  if (vtkPythonInterpreter::ConsoleBuffering)
  {
    vtkPythonInterpreter::StdOutBuffer += std::string(txt);
  }
  else
  {
    vtkOutputWindowDisplayText(txt);
    NotifyInterpreters(vtkCommand::SetOutputEvent, const_cast<char*>(txt));
  }
}

//----------------------------------------------------------------------------
void vtkPythonInterpreter::FlushStdOut()
{
}

//----------------------------------------------------------------------------
void vtkPythonInterpreter::WriteStdErr(const char* txt)
{
  if (vtkPythonInterpreter::ConsoleBuffering)
  {
    vtkPythonInterpreter::StdErrBuffer += std::string(txt);
  }
  else
  {
    vtkOutputWindowDisplayErrorText(txt);
    NotifyInterpreters(vtkCommand::ErrorEvent, const_cast<char*>(txt));
  }
}

//----------------------------------------------------------------------------
void vtkPythonInterpreter::FlushStdErr()
{
}

//----------------------------------------------------------------------------
vtkStdString vtkPythonInterpreter::ReadStdin()
{
  if (!vtkPythonInterpreter::CaptureStdin)
  {
    vtkStdString string;
    cin >> string;
    return string;
  }
  vtkStdString string;
  NotifyInterpreters(vtkCommand::UpdateEvent, &string);
  return string;
}

//----------------------------------------------------------------------------
void vtkPythonInterpreter::SetupPythonPrefix()
{
  using systools = vtksys::SystemTools;

  if (Py_GetPythonHome() != nullptr)
  {
    // if PYTHONHOME is set, we do nothing. Don't override an already
    // overridden environment.
    VTKPY_DEBUG_MESSAGE("`PYTHONHOME` already set. Leaving unchanged.");
    return;
  }

  std::string pythonlib = GetLibraryForSymbol("Py_SetProgramName");
  if (pythonlib.empty())
  {
    VTKPY_DEBUG_MESSAGE("static Python build or `Py_SetProgramName` library couldn't be found. "
                        "Set `PYTHONHOME` if Python standard library fails to load.");
    return;
  }

#if PY_VERSION_HEX >= 0x03000000
  auto oldprogramname = vtk_Py_EncodeLocale(Py_GetProgramName(), nullptr);
#else
  auto oldprogramname = Py_GetProgramName();
#endif
  if (oldprogramname != nullptr && strcmp(oldprogramname, "python") != 0)
  {
    VTKPY_DEBUG_MESSAGE("program-name has been changed. Leaving unchanged.");
#if PY_VERSION_HEX >= 0x03000000
    PyMem_Free(oldprogramname);
#endif
    return;
  }

#if PY_VERSION_HEX >= 0x03000000
  PyMem_Free(oldprogramname);
#endif

  const std::string newprogramname =
    systools::GetFilenamePath(pythonlib) + VTK_PATH_SEPARATOR "vtkpython";
  VTKPY_DEBUG_MESSAGE(
    "calling Py_SetProgramName(" << newprogramname << ") to aid in setup of Python prefix.");
#if PY_VERSION_HEX >= 0x03000000
  static WCharStringPool wpool;
  Py_SetProgramName(wpool.push_back(vtk_Py_DecodeLocale(newprogramname.c_str(), nullptr)));
#else
  static StringPool pool;
  Py_SetProgramName(pool.push_back(systools::DuplicateString(newprogramname.c_str())));
#endif
}

//----------------------------------------------------------------------------
void vtkPythonInterpreter::SetupVTKPythonPaths()
{
  using systools = vtksys::SystemTools;

#if defined(VTK_BUILD_SHARED_LIBS) // and !frozen_vtk_python
  // add path for VTK shared libs.
  VTKPY_DEBUG_MESSAGE("shared VTK build detected.");
  const std::string vtklib = GetLibraryForSymbol("GetVTKVersion");
  if (vtklib.empty())
  {
    VTKPY_DEBUG_MESSAGE(
      "`GetVTKVersion` library couldn't be found. Will use `Py_GetProgramName` next.");
  }
#else
  // static build.
  VTKPY_DEBUG_MESSAGE(
    "static VTK build detected. Using `Py_GetProgramName` to locate python modules.");
  const std::string vtklib;
#endif

  std::vector<std::string> vtkprefix_components;
  if (vtklib.empty())
  {
    std::string vtkprefix;
#if PY_VERSION_HEX >= 0x03000000
    auto tmp = vtk_Py_EncodeLocale(Py_GetProgramName(), nullptr);
    vtkprefix = tmp;
    PyMem_Free(tmp);
#else
    vtkprefix = Py_GetProgramName();
#endif
    vtkprefix = systools::CollapseFullPath(vtkprefix);
    systools::SplitPath(vtkprefix, vtkprefix_components);
  }
  else
  {
    systools::SplitPath(systools::GetFilenamePath(vtklib), vtkprefix_components);
  }

  const std::string sitepackages = VTK_PYTHON_SITE_PACKAGES_SUFFIX;
#if defined(_WIN32) && !defined(__CYGWIN__)
  const std::string landmark = "vtk\\__init__.py";
#else
  const std::string landmark = "vtk/__init__.py";
#endif

  while (!vtkprefix_components.empty())
  {
    std::string curprefix = systools::JoinPath(vtkprefix_components);
    const std::string pathtocheck = curprefix + VTK_PATH_SEPARATOR + sitepackages;
    const std::string landmarktocheck = pathtocheck + VTK_PATH_SEPARATOR + landmark;
    if (vtksys::SystemTools::FileExists(landmarktocheck))
    {
      VTKPY_DEBUG_MESSAGE_VV("trying VTK landmark file " << landmarktocheck << " -- success!");
      vtkSafePrependPythonPath(pathtocheck);
      break;
    }
    else
    {
      VTKPY_DEBUG_MESSAGE_VV("trying VTK landmark file " << landmarktocheck << " -- failed!");
    }
    vtkprefix_components.pop_back();
  }
}
