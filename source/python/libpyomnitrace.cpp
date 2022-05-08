// MIT License
//
// Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "libpyomnitrace.hpp"
#include "dl.hpp"
#include "library/coverage.hpp"
#include "library/impl/coverage.hpp"

#include <timemory/backends/process.hpp>
#include <timemory/backends/threading.hpp>
#include <timemory/environment.hpp>
#include <timemory/mpl/apply.hpp>
#include <timemory/mpl/policy.hpp>
#include <timemory/tpls/cereal/cereal.hpp>
#include <timemory/utility/filepath.hpp>
#include <timemory/utility/macros.hpp>
#include <timemory/utility/types.hpp>
#include <timemory/variadic/macros.hpp>

#include <pybind11/detail/common.h>
#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pyerrors.h>

#include <cctype>
#include <cstdint>
#include <exception>
#include <locale>
#include <regex>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace pyomnitrace
{
namespace pyprofile
{
py::module
generate(py::module& _pymod);
}
namespace pycoverage
{
py::module
generate(py::module& _pymod);
}
}  // namespace pyomnitrace

template <typename... Tp>
using uomap_t = std::unordered_map<Tp...>;

PYBIND11_MODULE(libpyomnitrace, omni)
{
    using namespace pyomnitrace;

    static bool _is_initialized = false;
    static bool _is_finalized   = false;
    static auto _get_use_mpi    = []() {
        bool _use_mpi = false;
        try
        {
            py::module::import("mpi4py");
            _use_mpi = true;
        } catch(py::error_already_set& _exc)
        {
            if(!_exc.matches(PyExc_ImportError)) throw;
        }
        return _use_mpi;
    };

    omni.def(
        "is_initialized", []() { return _is_initialized; }, "Initialization state");

    omni.def(
        "is_finalized", []() { return _is_finalized; }, "Finalization state");

    omni.def(
        "initialize",
        [](const std::string& _v) {
            if(_is_initialized)
                throw std::runtime_error("Error! omnitrace is already initialized");
            _is_initialized = true;
            omnitrace_set_mpi(_get_use_mpi(), false);
            omnitrace_init("trace", false, _v.c_str());
        },
        "Initialize omnitrace");

    omni.def(
        "initialize",
        [](const py::list& _v) {
            if(_is_initialized)
                throw std::runtime_error("Error! omnitrace is already initialized");
            _is_initialized = true;
            omnitrace_set_mpi(_get_use_mpi(), false);
            std::string _cmd      = {};
            std::string _cmd_line = {};
            for(auto&& itr : _v)
            {
                if(_cmd.empty()) _cmd = itr.cast<std::string>();
                _cmd_line += " " + itr.cast<std::string>();
            }
            if(!_cmd_line.empty())
            {
                _cmd_line.substr(_cmd_line.find_first_not_of(' '));
                tim::set_env("OMNITRACE_COMMAND_LINE", _cmd_line, 0);
            }
            omnitrace_init("trace", false, _cmd.c_str());
        },
        "Initialize omnitrace");

    omni.def(
        "initialize",
        [](const std::string& _v) {
            if(_is_initialized)
                throw std::runtime_error("Error! omnitrace is already initialized");
            _is_initialized = true;
            bool _use_mpi   = false;
            try
            {
                py::module::import("mpi4py");
                _use_mpi = true;
            } catch(py::error_already_set& _exc)
            {
                if(!_exc.matches(PyExc_ImportError)) throw;
            }
            omnitrace_set_mpi(_use_mpi, false);
            omnitrace_init("trace", false, _v.c_str());
        },
        "Initialize omnitrace");

    omni.def(
        "finalize",
        []() {
            if(_is_finalized)
                throw std::runtime_error("Error! omnitrace is already finalized");
            _is_finalized = true;
            omnitrace_finalize();
        },
        "Initialize omnitrace");

    py::doc("omnitrace profiler for python");
    pyprofile::generate(omni);

    pycoverage::generate(omni);
}

//======================================================================================//
//
namespace pyomnitrace
{
namespace pyprofile
{
//
using profiler_t           = std::function<void()>;
using profiler_vec_t       = std::vector<profiler_t>;
using profiler_label_map_t = std::unordered_map<std::string, profiler_vec_t>;
using profiler_index_map_t = std::unordered_map<uint32_t, profiler_label_map_t>;
using strset_t             = std::unordered_set<std::string>;
//
namespace
{
strset_t default_exclude_functions = { "^<.*>$" };
strset_t default_exclude_filenames = { "(encoder|decoder|threading).py$", "^<.*>$" };
}  // namespace
//
struct config
{
    bool                    is_running         = false;
    bool                    trace_c            = false;
    bool                    include_internal   = false;
    bool                    include_args       = false;
    bool                    include_line       = false;
    bool                    include_filename   = false;
    bool                    full_filepath      = false;
    int32_t                 ignore_stack_depth = 0;
    int32_t                 base_stack_depth   = -1;
    int32_t                 verbose            = 0;
    int64_t                 depth_tracker      = 0;
    std::string             base_module_path   = {};
    strset_t                restrict_functions = {};
    strset_t                restrict_filenames = {};
    strset_t                include_functions  = {};
    strset_t                include_filenames  = {};
    strset_t                exclude_functions  = default_exclude_functions;
    strset_t                exclude_filenames  = default_exclude_filenames;
    std::vector<profiler_t> records            = {};
};
//
inline config&
get_config()
{
    static auto*              _instance    = new config{};
    static thread_local auto* _tl_instance = []() {
        static std::atomic<uint32_t> _count{ 0 };
        auto                         _cnt = _count++;
        if(_cnt == 0) return _instance;

        auto* _tmp               = new config{};
        _tmp->is_running         = _instance->is_running;
        _tmp->trace_c            = _instance->trace_c;
        _tmp->include_internal   = _instance->include_internal;
        _tmp->include_args       = _instance->include_args;
        _tmp->include_line       = _instance->include_line;
        _tmp->include_filename   = _instance->include_filename;
        _tmp->full_filepath      = _instance->full_filepath;
        _tmp->base_module_path   = _instance->base_module_path;
        _tmp->restrict_functions = _instance->restrict_functions;
        _tmp->restrict_filenames = _instance->restrict_filenames;
        _tmp->include_functions  = _instance->include_functions;
        _tmp->include_filenames  = _instance->include_filenames;
        _tmp->exclude_functions  = _instance->exclude_functions;
        _tmp->exclude_filenames  = _instance->exclude_filenames;
        _tmp->verbose            = _instance->verbose;
        // if full filepath is specified, include filename is implied
        if(_tmp->full_filepath && !_tmp->include_filename) _tmp->include_filename = true;
        return _tmp;
    }();
    return *_tl_instance;
}
//
int32_t
get_depth(PyFrameObject* frame)
{
    return (frame->f_back) ? (get_depth(frame->f_back) + 1) : 0;
}
//
void
profiler_function(py::object pframe, const char* swhat, py::object arg)
{
    static thread_local auto& _config  = get_config();
    static thread_local auto  _disable = false;

    if(_disable) return;

    _disable = true;
    tim::scope::destructor _dtor{ []() { _disable= false; } };
    (void) _dtor;

    if(pframe.is_none() || pframe.ptr() == nullptr) return;

    static auto _omnitrace_path = _config.base_module_path;

    auto* frame = reinterpret_cast<PyFrameObject*>(pframe.ptr());

    int what = (strcmp(swhat, "call") == 0)       ? PyTrace_CALL
               : (strcmp(swhat, "c_call") == 0)   ? PyTrace_C_CALL
               : (strcmp(swhat, "return") == 0)   ? PyTrace_RETURN
               : (strcmp(swhat, "c_return") == 0) ? PyTrace_C_RETURN
                                                  : -1;
    // only support PyTrace_{CALL,C_CALL,RETURN,C_RETURN}
    if(what < 0)
    {
        if(_config.verbose > 2)
            TIMEMORY_PRINT_HERE("%s :: %s",
                                "Ignoring what != {CALL,C_CALL,RETURN,C_RETURN}", swhat);
        return;
    }

    auto _update_ignore_stack_depth = [what]() {
        switch(what)
        {
            case PyTrace_CALL: ++_config.ignore_stack_depth; break;
            case PyTrace_RETURN: --_config.ignore_stack_depth; break;
            default: break;
        }
    };

    if(_config.ignore_stack_depth > 0)
    {
        if(_config.verbose > 2)
            TIMEMORY_PRINT_HERE("%s :: %s :: %u", "Ignoring call/return", swhat,
                                _config.ignore_stack_depth);
        _update_ignore_stack_depth();
        return;
    }
    else if(_config.ignore_stack_depth < 0)
    {
        TIMEMORY_PRINT_HERE("WARNING! ignore_stack_depth is < 0 :: ",
                            _config.ignore_stack_depth);
    }

    // if PyTrace_C_{CALL,RETURN} is not enabled
    if(!_config.trace_c && (what == PyTrace_C_CALL || what == PyTrace_C_RETURN))
    {
        if(_config.verbose > 2)
            TIMEMORY_PRINT_HERE("%s :: %s", "Ignoring C call/return", swhat);
        return;
    }

    // get the arguments
    auto _get_args = [&]() {
        auto inspect = py::module::import("inspect");
        try
        {
            return py::cast<std::string>(
                inspect.attr("formatargvalues")(*inspect.attr("getargvalues")(pframe)));
        } catch(py::error_already_set& _exc)
        {
            TIMEMORY_CONDITIONAL_PRINT_HERE(_config.verbose > 1, "Error! %s",
                                            _exc.what());
            if(!_exc.matches(PyExc_AttributeError)) throw;
        }
        return std::string{};
    };

    // get the final label
    auto _get_label = [&](auto& _func, auto& _filename, auto& _fullpath) {
        auto _bracket = _config.include_filename;
        if(_bracket) _func.insert(0, "[");
        // append the arguments
        if(_config.include_args) _func.append(_get_args());
        if(_bracket) _func.append("]");
        // append the filename
        if(_config.include_filename)
        {
            if(_config.full_filepath)
                _func.append(TIMEMORY_JOIN("", '[', std::move(_fullpath)));
            else
                _func.append(TIMEMORY_JOIN("", '[', std::move(_filename)));
        }
        // append the line number
        if(_config.include_line && _config.include_filename)
            _func.append(TIMEMORY_JOIN("", ':', frame->f_lineno, ']'));
        else if(_config.include_line)
            _func.append(TIMEMORY_JOIN("", ':', frame->f_lineno));
        else if(_config.include_filename)
            _func += "]";
        return _func;
    };

    auto _find_matching = [](const strset_t& _expr, const std::string& _name) {
        const auto _rconstants =
            std::regex_constants::egrep | std::regex_constants::optimize;
        for(const auto& itr : _expr)  // NOLINT
        {
            if(std::regex_search(_name, std::regex(itr, _rconstants))) return true;
        }
        return false;
    };

    bool  _force      = false;
    auto& _only_funcs = _config.restrict_functions;
    auto& _incl_funcs = _config.include_functions;
    auto& _skip_funcs = _config.exclude_functions;
    auto  _func       = py::cast<std::string>(frame->f_code->co_name);

    if(!_only_funcs.empty())
    {
        _force = _find_matching(_only_funcs, _func);
        if(!_force)
        {
            if(_config.verbose > 2)
                TIMEMORY_PRINT_HERE("Skipping non-restricted function: %s",
                                    _func.c_str());
            return;
        }
    }

    if(!_force)
    {
        if(_find_matching(_incl_funcs, _func))
        {
            _force = true;
        }
        else if(_find_matching(_skip_funcs, _func))
        {
            if(_config.verbose > 1)
                TIMEMORY_PRINT_HERE("Skipping designated function: '%s'", _func.c_str());
            if(!_find_matching(default_exclude_functions, _func))
                _update_ignore_stack_depth();
            return;
        }
    }

    auto& _only_files = _config.restrict_filenames;
    auto& _incl_files = _config.include_filenames;
    auto& _skip_files = _config.exclude_filenames;
    auto  _full       = py::cast<std::string>(frame->f_code->co_filename);
    auto  _file       = (_full.find('/') != std::string::npos)
                            ? _full.substr(_full.find_last_of('/') + 1)
                            : _full;

    if(!_config.include_internal &&
       strncmp(_full.c_str(), _omnitrace_path.c_str(), _omnitrace_path.length()) == 0)
    {
        if(_config.verbose > 2)
            TIMEMORY_PRINT_HERE("Skipping internal function: %s", _func.c_str());
        return;
    }

    if(!_force && !_only_files.empty())
    {
        _force = _find_matching(_only_files, _full);
        if(!_force)
        {
            if(_config.verbose > 2)
                TIMEMORY_PRINT_HERE("Skipping non-restricted file: %s", _full.c_str());
            return;
        }
    }

    if(!_force)
    {
        if(_find_matching(_incl_files, _full))
        {
            _force = true;
        }
        else if(_find_matching(_skip_files, _full))
        {
            if(_config.verbose > 2)
                TIMEMORY_PRINT_HERE("Skipping non-included file: %s", _full.c_str());
            return;
        }
    }

    TIMEMORY_CONDITIONAL_PRINT_HERE(_config.verbose > 3, "%8s | %s%s | %s | %s", swhat,
                                    _func.c_str(), _get_args().c_str(), _file.c_str(),
                                    _full.c_str());

    auto _label = _get_label(_func, _file, _full);
    if(_label.empty()) return;

    static thread_local strset_t _labels{};
    const auto&                  _label_ref = *_labels.emplace(_label).first;

    // start function
    auto _profiler_call = [&]() {
        _config.records.emplace_back(
            [&_label_ref]() { omnitrace_pop_region(_label_ref.c_str()); });
        omnitrace_push_region(_label_ref.c_str());
    };

    // stop function
    auto _profiler_return = [&]() {
        _config.records.back()();
        _config.records.pop_back();
    };

    // process what
    switch(what)
    {
        case PyTrace_CALL:
        case PyTrace_C_CALL: _profiler_call(); break;
        case PyTrace_RETURN:
        case PyTrace_C_RETURN: _profiler_return(); break;
        default: break;
    }

    // don't do anything with arg
    tim::consume_parameters(arg);
}
//
py::module
generate(py::module& _pymod)
{
    py::module _prof = _pymod.def_submodule("profiler", "Profiling functions");

    auto _init = []() {
        try
        {
            auto _file =
                py::module::import("omnitrace").attr("__file__").cast<std::string>();
            if(_file.find('/') != std::string::npos)
                _file = _file.substr(0, _file.find_last_of('/'));
            get_config().base_module_path = _file;
        } catch(py::cast_error& e)
        {
            std::cerr << "[profiler_init]> " << e.what() << std::endl;
        }
        if(get_config().is_running) return;
        get_config().records.clear();
        get_config().base_stack_depth = -1;
        get_config().is_running       = true;
    };

    auto _fini = []() {
        if(!get_config().is_running) return;
        get_config().is_running       = false;
        get_config().base_stack_depth = -1;
        get_config().records.clear();
    };

    _prof.def("profiler_function", &profiler_function, "Profiling function");
    _prof.def("profiler_init", _init, "Initialize the profiler");
    _prof.def("profiler_finalize", _fini, "Finalize the profiler");

    py::class_<config> _pyconfig(_prof, "config", "Profiler configuration");

#define CONFIGURATION_PROPERTY(NAME, TYPE, DOC, ...)                                     \
    _pyconfig.def_property_static(                                                       \
        NAME, [](py::object&&) { return __VA_ARGS__; },                                  \
        [](py::object&&, TYPE val) { __VA_ARGS__ = val; }, DOC);

    CONFIGURATION_PROPERTY("_is_running", bool, "Profiler is currently running",
                           get_config().is_running)
    CONFIGURATION_PROPERTY("trace_c", bool, "Enable tracing C functions",
                           get_config().trace_c)
    CONFIGURATION_PROPERTY("include_internal", bool, "Include functions within timemory",
                           get_config().include_internal)
    CONFIGURATION_PROPERTY("include_args", bool, "Encode the function arguments",
                           get_config().include_args)
    CONFIGURATION_PROPERTY("include_line", bool, "Encode the function line number",
                           get_config().include_line)
    CONFIGURATION_PROPERTY("include_filename", bool,
                           "Encode the function filename (see also: full_filepath)",
                           get_config().include_filename)
    CONFIGURATION_PROPERTY("full_filepath", bool,
                           "Display the full filepath (instead of file basename)",
                           get_config().full_filepath)
    CONFIGURATION_PROPERTY("verbosity", int32_t, "Verbosity of the logging",
                           get_config().verbose)

    static auto _get_strset = [](const strset_t& _targ) {
        auto _out = py::list{};
        for(auto itr : _targ)
            _out.append(itr);
        return _out;
    };

    static auto _set_strset = [](const py::list& _inp, strset_t& _targ) {
        for(const auto& itr : _inp)
            _targ.insert(itr.cast<std::string>());
    };

#define CONFIGURATION_PROPERTY_LAMBDA(NAME, DOC, GET, SET)                               \
    _pyconfig.def_property_static(NAME, GET, SET, DOC);
#define CONFIGURATION_STRSET(NAME, DOC, ...)                                             \
    {                                                                                    \
        auto GET = [](py::object&&) { return _get_strset(__VA_ARGS__); };                \
        auto SET = [](py::object&&, const py::list& val) {                               \
            _set_strset(val, __VA_ARGS__);                                               \
        };                                                                               \
        CONFIGURATION_PROPERTY_LAMBDA(NAME, DOC, GET, SET)                               \
    }

    CONFIGURATION_STRSET("restrict_functions", "Function regexes to collect exclusively",
                         get_config().restrict_functions)
    CONFIGURATION_STRSET("restrict_modules", "Filename regexes to collect exclusively",
                         get_config().restrict_filenames)
    CONFIGURATION_STRSET("include_functions",
                         "Function regexes to always include in collection",
                         get_config().include_functions)
    CONFIGURATION_STRSET("include_modules",
                         "Filename regexes to always include in collection",
                         get_config().include_filenames)
    CONFIGURATION_STRSET("exclude_functions",
                         "Function regexes to filter out of collection",
                         get_config().exclude_functions)
    CONFIGURATION_STRSET("exclude_modules",
                         "Filename regexes to filter out of collection",
                         get_config().exclude_filenames)

    return _prof;
}
}  // namespace pyprofile

namespace pycoverage
{
py::module
generate(py::module& _pymod)
{
    namespace coverage           = omnitrace::coverage;
    using coverage_data_vector_t = std::vector<coverage::coverage_data>;

    py::module _pycov = _pymod.def_submodule("coverage", "Code coverage");

#define DEFINE_PROPERTY(PYCLASS, CLASS, TYPE, NAME, ...)                                 \
    PYCLASS.def_property(                                                                \
        #NAME, [](CLASS* _object) { return _object->NAME; },                             \
        [](CLASS* _object, TYPE v) { return _object->NAME = v; }, __VA_ARGS__)

    py::class_<coverage::code_coverage> _pycov_summary{ _pymod, "summary",
                                                        "Code coverage summary" };

    _pycov_summary.def(py::init([]() { return new coverage::code_coverage{}; }),
                       "Create a default instance");

    _pycov_summary.def(
        "get_code_coverage",
        [](coverage::code_coverage* _v) {
            return _v->get(coverage::code_coverage::STANDARD);
        },
        "Get coverage fraction");
    _pycov_summary.def(
        "get_module_coverage",
        [](coverage::code_coverage* _v) {
            return _v->get(coverage::code_coverage::MODULE);
        },
        "Get coverage fraction");
    _pycov_summary.def(
        "get_function_coverage",
        [](coverage::code_coverage* _v) {
            return _v->get(coverage::code_coverage::FUNCTION);
        },
        "Get coverage fraction");
    _pycov_summary.def("get_uncovered_modules",
                       &coverage::code_coverage::get_uncovered_modules,
                       "List of uncovered modules");
    _pycov_summary.def("get_uncovered_functions",
                       &coverage::code_coverage::get_uncovered_functions,
                       "List of uncovered functions");

    DEFINE_PROPERTY(_pycov_summary, coverage::code_coverage, size_t, count,
                    "Number of times covered");
    DEFINE_PROPERTY(_pycov_summary, coverage::code_coverage, size_t, size,
                    "Total number of coverage entries");
    DEFINE_PROPERTY(_pycov_summary, coverage::code_coverage,
                    coverage::code_coverage::data, covered, "Covered information");
    DEFINE_PROPERTY(_pycov_summary, coverage::code_coverage,
                    coverage::code_coverage::data, possible, "Possible information");

    py::class_<coverage::code_coverage::data> _pycov_summary_data{ _pycov_summary, "data",
                                                                   "Code coverage data" };

    DEFINE_PROPERTY(_pycov_summary_data, coverage::code_coverage::data, std::set<size_t>,
                    addresses, "Addresses");
    DEFINE_PROPERTY(_pycov_summary_data, coverage::code_coverage::data,
                    std::set<std::string>, modules, "Modules");
    DEFINE_PROPERTY(_pycov_summary_data, coverage::code_coverage::data,
                    std::set<std::string>, functions, "Functions");

    py::class_<coverage::coverage_data> _pycov_details{ _pycov, "details",
                                                        "Code coverage data" };

    _pycov_details.def(py::init([]() { return new coverage::coverage_data{}; }),
                       "Create a default instance");

    DEFINE_PROPERTY(_pycov_details, coverage::coverage_data, size_t, count,
                    "Number of times invoked");
    DEFINE_PROPERTY(_pycov_details, coverage::coverage_data, size_t, address,
                    "Address of coverage entity (in binary)");
    DEFINE_PROPERTY(_pycov_details, coverage::coverage_data, size_t, line,
                    "Line number of coverage entity");
    DEFINE_PROPERTY(_pycov_details, coverage::coverage_data, std::string, module,
                    "Name of the containing module");
    DEFINE_PROPERTY(_pycov_details, coverage::coverage_data, std::string, function,
                    "Name of the function (basic)");
    DEFINE_PROPERTY(_pycov_details, coverage::coverage_data, std::string, source,
                    "Full signature of the function");

    _pycov_details.def(py::self + py::self);
    _pycov_details.def(py::self += py::self);
    _pycov_details.def(py::self == py::self);
    _pycov_details.def(py::self != py::self);
    _pycov_details.def(py::self < py::self);
    _pycov_details.def(py::self > py::self);
    _pycov_details.def(py::self <= py::self);
    _pycov_details.def(py::self >= py::self);

    auto _load_coverage = [](const std::string& _inp) {
        coverage::code_coverage* _summary = nullptr;
        coverage_data_vector_t*  _details = nullptr;
        std::ifstream            ifs{ _inp };
        if(ifs)
        {
            namespace cereal = tim::cereal;
            auto ar = tim::policy::input_archive<cereal::JSONInputArchive>::get(ifs);

            try
            {
                ar->setNextName("omnitrace");
                ar->startNode();
                ar->setNextName("coverage");
                ar->startNode();
                _summary = new coverage::code_coverage{};
                _details = new coverage_data_vector_t{};
                (*ar)(cereal::make_nvp("summary", *_summary));
                (*ar)(cereal::make_nvp("details", *_details));
                ar->finishNode();
                ar->finishNode();
            } catch(std::exception&)
            {}
        }
        return std::make_tuple(_summary, _details);
    };

    auto _save_coverage = [](coverage::code_coverage* _summary,
                             coverage_data_vector_t* _details, std::string _name) {
        std::stringstream oss{};
        {
            namespace cereal = tim::cereal;
            auto ar =
                tim::policy::output_archive<cereal::PrettyJSONOutputArchive>::get(oss);

            ar->setNextName("omnitrace");
            ar->startNode();
            ar->setNextName("coverage");
            ar->startNode();
            (*ar)(cereal::make_nvp("summary", *_summary));
            (*ar)(cereal::make_nvp("details", *_details));
            ar->finishNode();
            ar->finishNode();
        }
        _name = TIMEMORY_JOIN(
            '.', std::regex_replace(_name, std::regex{ "(.*)(\\.json$)" }, "$1"), "json");
        std::ofstream ofs{};
        if(tim::filepath::open(ofs, _name))
        {
            fprintf(stderr, "[%s][coverage]> Outputting '%s'...\n", TIMEMORY_PROJECT_NAME,
                    _name.c_str());
            ofs << oss.str() << "\n";
        }
        else
        {
            throw std::runtime_error(
                TIMEMORY_JOIN("", "Error opening coverage output file: ", _name));
        }
    };

    _pycov.def("load", _load_coverage, "Load code coverage data");
    _pycov.def("save", _save_coverage, "Save code coverage data", py::arg("summary"),
               py::arg("details"), py::arg("filename") = "coverage.json");

    auto _concat_coverage = [](coverage_data_vector_t* _lhs,
                               coverage_data_vector_t* _rhs) {
        std::sort(_rhs->begin(), _rhs->end(), std::greater<coverage::coverage_data>{});

        auto _find = [_lhs](const auto& _v) {
            for(auto itr = _lhs->begin(); itr != _lhs->end(); ++itr)
            {
                if(*itr == _v) return std::make_pair(itr, true);
            }
            return std::make_pair(_lhs->end(), false);
        };

        std::vector<coverage::coverage_data*> _new_entries{};
        _new_entries.reserve(_rhs->size());
        for(auto& itr : *_rhs)
        {
            auto litr = _find(itr);
            if(!litr.second)
                _new_entries.emplace_back(&itr);
            else
                *litr.first += itr;
        }

        _lhs->reserve(_lhs->size() + _new_entries.size());
        for(auto& itr : _new_entries)
            _lhs->emplace_back(std::move(*itr));
        _rhs->clear();

        std::sort(_lhs->begin(), _lhs->end(), std::greater<coverage::coverage_data>{});
        return _lhs;
    };

    _pycov.def("concat", _concat_coverage, "Combined code coverage details");

    using coverage_data_map =
        uomap_t<std::string_view, uomap_t<std::string_view, std::map<size_t, size_t>>>;

    auto _coverage_summary = [](coverage_data_vector_t* _data) {
        coverage::code_coverage _summary{};
        coverage_data_map       _mdata{};

        for(auto& itr : *_data)
            _mdata[itr.module][itr.function][itr.address] += itr.count;

        for(const auto& file : _mdata)
        {
            for(const auto& func : file.second)
            {
                for(const auto& addr : func.second)
                {
                    if(addr.second > 0)
                    {
                        _summary.count += 1;
                        _summary.covered.modules.emplace(file.first);
                        _summary.covered.functions.emplace(func.first);
                        _summary.covered.addresses.emplace(addr.first);
                    }
                    _summary.size += 1;
                    _summary.possible.modules.emplace(file.first);
                    _summary.possible.functions.emplace(func.first);
                    _summary.possible.addresses.emplace(addr.first);
                }
            }
        }

        return _summary;
    };

    _pycov.def("get_summary", _coverage_summary, "Generate a code coverage summary");

    auto _get_top = [](coverage_data_vector_t* _data, size_t _n) {
        auto _ret = *_data;
        std::sort(_ret.begin(), _ret.end(), std::greater<coverage::coverage_data>{});
        _ret.resize(std::min<size_t>(_n, _ret.size()));
        _ret.shrink_to_fit();
        return _ret;
    };

    _pycov.def("get_top", _get_top, "Get the top covered functions", py::arg("details"),
               py::arg("n") = 10);

    auto _get_bottom = [](coverage_data_vector_t* _data, size_t _n) {
        auto _ret = *_data;
        std::sort(_ret.begin(), _ret.end(), std::less<coverage::coverage_data>{});
        _ret.resize(std::min<size_t>(_n, _ret.size()));
        _ret.shrink_to_fit();
        return _ret;
    };

    _pycov.def("get_bottom", _get_bottom, "Get the bottom covered functions",
               py::arg("details"), py::arg("n") = 10);

    return _pycov;
}
}  // namespace pycoverage
}  // namespace pyomnitrace
//
//======================================================================================//