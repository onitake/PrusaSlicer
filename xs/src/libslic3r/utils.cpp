#include "Utils.hpp"
#include "I18N.hpp"

#include <locale>
#include <ctime>

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>

#include <boost/locale.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/date_time/local_time/local_time.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/integration/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdio.hpp>

#include <tbb/task_scheduler_init.h>

namespace Slic3r {

static boost::log::trivial::severity_level logSeverity = boost::log::trivial::error;

void set_logging_level(unsigned int level)
{
    switch (level) {
    // Report fatal errors only.
    case 0: logSeverity = boost::log::trivial::fatal; break;
    // Report fatal errors and errors.
    case 1: logSeverity = boost::log::trivial::error; break;
    // Report fatal errors, errors and warnings.
    case 2: logSeverity = boost::log::trivial::warning; break;
    // Report all errors, warnings and infos.
    case 3: logSeverity = boost::log::trivial::info; break;
    // Report all errors, warnings, infos and debugging.
    case 4: logSeverity = boost::log::trivial::debug; break;
    // Report everyting including fine level tracing information.
    default: logSeverity = boost::log::trivial::trace; break;
    }

    boost::log::core::get()->set_filter
    (
        boost::log::trivial::severity >= logSeverity
    );
}

// Force set_logging_level(<=error) after loading of the DLL.
// Switch boost::filesystem to utf8.
static struct RunOnInit {
    RunOnInit() { 
        boost::nowide::nowide_filesystem();
        set_logging_level(1);
    }
} g_RunOnInit;

void trace(unsigned int level, const char *message)
{
    boost::log::trivial::severity_level severity = boost::log::trivial::trace;
    switch (level) {
    // Report fatal errors only.
    case 0: severity = boost::log::trivial::fatal; break;
    // Report fatal errors and errors.
    case 1: severity = boost::log::trivial::error; break;
    // Report fatal errors, errors and warnings.
    case 2: severity = boost::log::trivial::warning; break;
    // Report all errors, warnings and infos.
    case 3: severity = boost::log::trivial::info; break;
    // Report all errors, warnings, infos and debugging.
    case 4: severity = boost::log::trivial::debug; break;
    // Report everyting including fine level tracing information.
    default: severity = boost::log::trivial::trace; break;
    }

    BOOST_LOG_STREAM_WITH_PARAMS(::boost::log::trivial::logger::get(),\
        (::boost::log::keywords::severity = severity)) << message;
}

void disable_multi_threading()
{
    // Disable parallelization so the Shiny profiler works
    static tbb::task_scheduler_init *tbb_init = nullptr;
    if (tbb_init == nullptr)
        tbb_init = new tbb::task_scheduler_init(1);
}

static std::string g_var_dir;

void set_var_dir(const std::string &dir)
{
    g_var_dir = dir;
}

const std::string& var_dir()
{
    return g_var_dir;
}

std::string var(const std::string &file_name)
{
    auto file = (boost::filesystem::path(g_var_dir) / file_name).make_preferred();
    return file.string();
}

static std::string g_resources_dir;

void set_resources_dir(const std::string &dir)
{
    g_resources_dir = dir;
}

const std::string& resources_dir()
{
    return g_resources_dir;
}

static std::string g_local_dir;

void set_local_dir(const std::string &dir)
{
    g_local_dir = dir;
}

const std::string& localization_dir()
{
	return g_local_dir;
}

// Translate function callback, to call wxWidgets translate function to convert non-localized UTF8 string to a localized one.
Slic3r::I18N::translate_fn_type Slic3r::I18N::translate_fn = nullptr;

static std::string g_data_dir;

void set_data_dir(const std::string &dir)
{
    g_data_dir = dir;
}

const std::string& data_dir()
{
    return g_data_dir;
}

// borrowed from LLVM lib/Support/Windows/Path.inc
int rename_file(const std::string &from, const std::string &to)
{
    int ec = 0;

#ifdef _WIN32

    // Convert to utf-16.
    std::wstring wide_from = boost::nowide::widen(from);
    std::wstring wide_to   = boost::nowide::widen(to);

    // Retry while we see recoverable errors.
    // System scanners (eg. indexer) might open the source file when it is written
    // and closed.
    bool TryReplace = true;

    // This loop may take more than 2000 x 1ms to finish.
    for (int i = 0; i < 2000; ++ i) {
        if (i > 0)
            // Sleep 1ms
            ::Sleep(1);
        if (TryReplace) {
            // Try ReplaceFile first, as it is able to associate a new data stream
            // with the destination even if the destination file is currently open.
            if (::ReplaceFileW(wide_to.data(), wide_from.data(), NULL, 0, NULL, NULL))
                return 0;
            DWORD ReplaceError = ::GetLastError();
            ec = -1; // ReplaceError
            // If ReplaceFileW returned ERROR_UNABLE_TO_MOVE_REPLACEMENT or
            // ERROR_UNABLE_TO_MOVE_REPLACEMENT_2, retry but only use MoveFileExW().
            if (ReplaceError == ERROR_UNABLE_TO_MOVE_REPLACEMENT ||
                ReplaceError == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
                TryReplace = false;
                continue;
            }
            // If ReplaceFileW returned ERROR_UNABLE_TO_REMOVE_REPLACED, retry
            // using ReplaceFileW().
            if (ReplaceError == ERROR_UNABLE_TO_REMOVE_REPLACED)
                continue;
            // We get ERROR_FILE_NOT_FOUND if the destination file is missing.
            // MoveFileEx can handle this case.
            if (ReplaceError != ERROR_ACCESS_DENIED && ReplaceError != ERROR_FILE_NOT_FOUND && ReplaceError != ERROR_SHARING_VIOLATION)
                break;
        }
        if (::MoveFileExW(wide_from.c_str(), wide_to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING))
            return 0;
        DWORD MoveError = ::GetLastError();
        ec = -1; // MoveError
        if (MoveError != ERROR_ACCESS_DENIED && MoveError != ERROR_SHARING_VIOLATION)
            break;
    }

#else

    boost::nowide::remove(to.c_str());
    ec = boost::nowide::rename(from.c_str(), to.c_str());

#endif

    return ec;
}

// Ignore system and hidden files, which may be created by the DropBox synchronisation process.
// https://github.com/prusa3d/Slic3r/issues/1298
bool is_plain_file(const boost::filesystem::directory_entry &dir_entry)
{
    if (! boost::filesystem::is_regular_file(dir_entry.status()))
        return false;
#ifdef _MSC_VER
    DWORD attributes = GetFileAttributesW(boost::nowide::widen(dir_entry.path().string()).c_str());
    return (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == 0;
#else
    return true;
#endif
}

bool is_ini_file(const boost::filesystem::directory_entry &dir_entry)
{
    return is_plain_file(dir_entry) && strcasecmp(dir_entry.path().extension().string().c_str(), ".ini") == 0;
}

bool is_idx_file(const boost::filesystem::directory_entry &dir_entry)
{
    return is_plain_file(dir_entry) && strcasecmp(dir_entry.path().extension().string().c_str(), ".idx") == 0;
}

} // namespace Slic3r

#include <xsinit.h>

void
confess_at(const char *file, int line, const char *func,
            const char *pat, ...)
{
    #ifdef SLIC3RXS
     va_list args;
     SV *error_sv = newSVpvf("Error in function %s at %s:%d: ", func,
         file, line);

     va_start(args, pat);
     sv_vcatpvf(error_sv, pat, &args);
     va_end(args);

     sv_catpvn(error_sv, "\n\t", 2);

     dSP;
     ENTER;
     SAVETMPS;
     PUSHMARK(SP);
     XPUSHs( sv_2mortal(error_sv) );
     PUTBACK;
     call_pv("Carp::confess", G_DISCARD);
     FREETMPS;
     LEAVE;
    #endif
}

void PerlCallback::register_callback(void *sv)
{ 
    if (! SvROK((SV*)sv) || SvTYPE(SvRV((SV*)sv)) != SVt_PVCV)
        croak("Not a Callback %_ for PerlFunction", (SV*)sv);
    if (m_callback)
        SvSetSV((SV*)m_callback, (SV*)sv);
    else
        m_callback = newSVsv((SV*)sv);
}

void PerlCallback::deregister_callback()
{
	if (m_callback) {
		sv_2mortal((SV*)m_callback);
		m_callback = nullptr;
	}
}

void PerlCallback::call() const
{
    if (! m_callback)
        return;
    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    PUTBACK; 
    perl_call_sv(SvRV((SV*)m_callback), G_DISCARD);
    FREETMPS;
    LEAVE;
}

void PerlCallback::call(int i) const
{
    if (! m_callback)
        return;
    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    XPUSHs(sv_2mortal(newSViv(i)));
    PUTBACK; 
    perl_call_sv(SvRV((SV*)m_callback), G_DISCARD);
    FREETMPS;
    LEAVE;
}

void PerlCallback::call(int i, int j) const
{
    if (! m_callback)
        return;
    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    XPUSHs(sv_2mortal(newSViv(i)));
    XPUSHs(sv_2mortal(newSViv(j)));
    PUTBACK; 
    perl_call_sv(SvRV((SV*)m_callback), G_DISCARD);
    FREETMPS;
    LEAVE;
}

void PerlCallback::call(const std::vector<int>& ints) const
{
    if (! m_callback)
        return;
    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    for (int i : ints)
    {
        XPUSHs(sv_2mortal(newSViv(i)));
    }
    PUTBACK;
    perl_call_sv(SvRV((SV*)m_callback), G_DISCARD);
    FREETMPS;
    LEAVE;
}

void PerlCallback::call(double d) const
{
    if (!m_callback)
        return;
    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    XPUSHs(sv_2mortal(newSVnv(d)));
    PUTBACK;
    perl_call_sv(SvRV((SV*)m_callback), G_DISCARD);
    FREETMPS;
    LEAVE;
}

void PerlCallback::call(double a, double b) const
{
    if (!m_callback)
        return;
    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    XPUSHs(sv_2mortal(newSVnv(a)));
    XPUSHs(sv_2mortal(newSVnv(b)));
    PUTBACK;
    perl_call_sv(SvRV((SV*)m_callback), G_DISCARD);
    FREETMPS;
    LEAVE;
}

void PerlCallback::call(double a, double b, double c, double d) const
{
    if (!m_callback)
        return;
    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    XPUSHs(sv_2mortal(newSVnv(a)));
    XPUSHs(sv_2mortal(newSVnv(b)));
    XPUSHs(sv_2mortal(newSVnv(c)));
    XPUSHs(sv_2mortal(newSVnv(d)));
    PUTBACK;
    perl_call_sv(SvRV((SV*)m_callback), G_DISCARD);
    FREETMPS;
    LEAVE;
}

void PerlCallback::call(bool b) const
{
    call(b ? 1 : 0);
}

#ifdef WIN32
    #ifndef NOMINMAX
    # define NOMINMAX
    #endif
    #include <windows.h>
#endif /* WIN32 */

namespace Slic3r {

// Encode an UTF-8 string to the local code page.
std::string encode_path(const char *src)
{    
#ifdef WIN32
    // Convert the source utf8 encoded string to a wide string.
    std::wstring wstr_src = boost::nowide::widen(src);
    if (wstr_src.length() == 0)
        return std::string();
    // Convert a wide string to a local code page.
    int size_needed = ::WideCharToMultiByte(0, 0, wstr_src.data(), (int)wstr_src.size(), nullptr, 0, nullptr, nullptr);
    std::string str_dst(size_needed, 0);
    ::WideCharToMultiByte(0, 0, wstr_src.data(), (int)wstr_src.size(), const_cast<char*>(str_dst.data()), size_needed, nullptr, nullptr);
    return str_dst;
#else /* WIN32 */
    return src;
#endif /* WIN32 */
}

// Encode an 8-bit string from a local code page to UTF-8.
std::string decode_path(const char *src)
{  
#ifdef WIN32
    int len = int(strlen(src));
    if (len == 0)
        return std::string();
    // Convert the string encoded using the local code page to a wide string.
    int size_needed = ::MultiByteToWideChar(0, 0, src, len, nullptr, 0);
    std::wstring wstr_dst(size_needed, 0);
    ::MultiByteToWideChar(0, 0, src, len, const_cast<wchar_t*>(wstr_dst.data()), size_needed);
    // Convert a wide string to utf8.
    return boost::nowide::narrow(wstr_dst.c_str());
#else /* WIN32 */
    return src;
#endif /* WIN32 */
}

std::string normalize_utf8_nfc(const char *src)
{
    static std::locale locale_utf8(boost::locale::generator().generate(""));
    return boost::locale::normalize(src, boost::locale::norm_nfc, locale_utf8);
}

namespace PerlUtils {
    // Get a file name including the extension.
    std::string path_to_filename(const char *src)       { return boost::filesystem::path(src).filename().string(); }
    // Get a file name without the extension.
    std::string path_to_stem(const char *src)           { return boost::filesystem::path(src).stem().string(); }
    // Get just the extension.
    std::string path_to_extension(const char *src)      { return boost::filesystem::path(src).extension().string(); }
    // Get a directory without the trailing slash.
    std::string path_to_parent_path(const char *src)    { return boost::filesystem::path(src).parent_path().string(); }
};

std::string timestamp_str()
{
    const auto now = boost::posix_time::second_clock::local_time();
    char buf[2048];
    sprintf(buf, "on %04d-%02d-%02d at %02d:%02d:%02d",
        // Local date in an ANSII format.
        int(now.date().year()), int(now.date().month()), int(now.date().day()),
        int(now.time_of_day().hours()), int(now.time_of_day().minutes()), int(now.time_of_day().seconds()));
    return buf;
}

unsigned get_current_pid()
{
#ifdef WIN32
    return GetCurrentProcessId();
#else
    return ::getpid();
#endif
}

std::string xml_escape(std::string text)
{
    std::string::size_type pos = 0;
    for (;;)
    {
        pos = text.find_first_of("\"\'&<>", pos);
        if (pos == std::string::npos)
            break;

        std::string replacement;
        switch (text[pos])
        {
        case '\"': replacement = "&quot;"; break;
        case '\'': replacement = "&apos;"; break;
        case '&':  replacement = "&amp;";  break;
        case '<':  replacement = "&lt;";   break;
        case '>':  replacement = "&gt;";   break;
        default: break;
        }

        text.replace(pos, 1, replacement);
        pos += replacement.size();
    }

    return text;
}

}; // namespace Slic3r
