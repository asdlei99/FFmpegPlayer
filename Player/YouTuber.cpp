#include "stdafx.h"

#include "YouTuber.h"

//#define YOUTUBE_EXPERIMENT

#ifdef YOUTUBE_EXPERIMENT

#include <Shlobj.h>

#include <boost/python/exec.hpp>
#include <boost/python/import.hpp>
#include <boost/python/extract.hpp>
#include <boost/python.hpp>

#include <boost/log/trivial.hpp>
#include <boost/log/sources/channel_logger.hpp>

#include <regex>
#include <fstream>
#include <iterator>
#include <streambuf>
#include <algorithm>
#include <memory>
#include <utility>

#include <tchar.h>

#include "unzip.h"
#include "http_get.h"

#include "MemoryMappedFile.h"

namespace {

// http://thejosephturner.com/blog/post/embedding-python-in-c-applications-with-boostpython-part-2/
// Parses the value of the active python exception
// NOTE SHOULD NOT BE CALLED IF NO EXCEPTION
std::string parse_python_exception()
{

    namespace py = boost::python;

    PyObject *type_ptr = NULL, *value_ptr = NULL, *traceback_ptr = NULL;
    // Fetch the exception info from the Python C API
    PyErr_Fetch(&type_ptr, &value_ptr, &traceback_ptr);

    // Fallback error
    std::string ret("Unfetchable Python error");
    // If the fetch got a type pointer, parse the type into the exception string
    if(type_ptr != NULL){
        py::handle<> h_type(type_ptr);
        py::str type_pstr(h_type);
        // Extract the string from the boost::python object
        py::extract<std::string> e_type_pstr(type_pstr);
        // If a valid string extraction is available, use it 
        //  otherwise use fallback
        if(e_type_pstr.check())
            ret = e_type_pstr();
        else
            ret = "Unknown exception type";
    }
    // Do the same for the exception value (the stringification of the exception)
    if(value_ptr != NULL){
        py::handle<> h_val(value_ptr);
        py::str a(h_val);
        py::extract<std::string> returned(a);
        if(returned.check())
            ret +=  ": " + returned();
        else
            ret += std::string(": Unparseable Python error: ");
    }
    // Parse lines from the traceback using the Python traceback module
    if(traceback_ptr != NULL){
        py::handle<> h_tb(traceback_ptr);
        // Load the traceback module and the format_tb function
        py::object tb(py::import("traceback"));
        py::object fmt_tb(tb.attr("format_tb"));
        // Call format_tb to get a list of traceback strings
        py::object tb_list(fmt_tb(h_tb));
        // Join the traceback strings into a single string
        py::object tb_str(py::str("\n").join(tb_list));
        // Extract the string, check the extraction, and fallback in necessary
        py::extract<std::string> returned(tb_str);
        if(returned.check())
            ret += ": " + returned();
        else
            ret += std::string(": Unparseable Python traceback");
    }
    return ret;
}


class LoggerStream
{
public:
    void write(const std::string& what)
    {
        using namespace boost::log;
        BOOST_LOG(sources::channel_logger_mt<>(keywords::channel = "python")) << what;
    }
    void flush() {}
};


const char PYTUBE_URL[] = "https://github.com/nficano/pytube/archive/master.zip";
const char YOUTUBE_TRANSCRIPT_API_URL[] = "https://github.com/jdepoix/youtube-transcript-api/archive/master.zip";

const char SCRIPT_TEMPLATE[] = R"(import sys
sys.stderr = LoggerStream()
sys.path.append("%s")
from pytube import YouTube
def getYoutubeUrl(url):
	return YouTube(url).streams.filter(progressive=True).order_by('resolution').desc().first().url)";

const char TRANSCRIPT_TEMPLATE[] = R"(import sys
sys.stderr = LoggerStream()

def install_and_import(package):
    import importlib
    try:
        importlib.import_module(package)
    except ImportError:
        import subprocess
        subprocess.call(["pip", "install", package])
    finally:
        globals()[package] = importlib.import_module(package)

install_and_import('requests')
sys.path.append("%s")
from youtube_transcript_api import YouTubeTranscriptApi
def getYoutubeTranscript(video_id):
	return YouTubeTranscriptApi.get_transcript(video_id))";

int from_hex(char ch)
{
    return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

std::string UrlUnescapeString(const std::string& s)
{
    std::istringstream ss(s);
    std::string result;
    std::getline(ss, result, '%');
    std::string buffer;
    while (std::getline(ss, buffer, '%'))
    {
        if (buffer.size() >= 2)
        {
            result += char((from_hex(buffer[0]) << 4) | from_hex(buffer[1])) + buffer.substr(2);
        }
    }
    return result;
}

bool extractYoutubeUrl(std::string& s)
{
    std::regex txt_regex(R"((http(s)?:\/\/)?((w){3}.)?youtu(be|.be)?(\.com)?\/.+)");
    std::string copy = s;
    for (int unescaped = 0; unescaped < 2; ++unescaped)
    {
        std::smatch m;
        if (std::regex_search(copy, m, txt_regex))
        {
            s = copy.substr(m.position());
            return true;
        }
        if (!unescaped)
            copy = UrlUnescapeString(copy);
    }

    return false;
}

bool extractYoutubeId(std::string& s)
{
    std::regex txt_regex(R"((?:v=|\/)([0-9A-Za-z_-]{11}).*)");
    std::string copy = s;
    for (int unescaped = 0; unescaped < 2; ++unescaped)
    {
        std::smatch m;
        if (std::regex_search(copy, m, txt_regex) && m.size() == 2)
        {
            s = m[1];//copy.substr(m.position());
            return true;
        }
        if (!unescaped)
            copy = UrlUnescapeString(copy);
    }

    return false;
}

bool DownloadAndExtractZip(const char* zipfile, const TCHAR* root)
{
    unzFile uf = unzOpen((voidpf)zipfile);
    if (!uf)
    {
        return false;
    }
    unzGoToFirstFile(uf);
    do {
        char filename[MAX_PATH];
        unzGetCurrentFileInfo(uf, 0, filename, sizeof(filename), 0, 0, 0, 0);

        TCHAR path[MAX_PATH];
        _tcscpy_s(path, root);
        PathAppend(path, CA2T(filename, CP_UTF8));

        auto pathlen = _tcslen(path);
        if (pathlen > 0 && (path[pathlen - 1] == _T('/') || path[pathlen - 1] == _T('\\')))
        {
            if (_tmkdir(path) != 0)
                return false;
        }
        else
        {
            unzOpenCurrentFile(uf);

            std::ofstream f(path, std::ofstream::binary);

            char buf[1024 * 64];
            int r;
            do
            {
                r = unzReadCurrentFile(uf, buf, sizeof(buf));
                if (r > 0)
                {
                    f.write(buf, r);
                }
            } while (r > 0);
            unzCloseCurrentFile(uf);
        }
    } while (unzGoToNextFile(uf) == UNZ_OK);

    unzClose(uf);
    return true;
}

std::string getPathWithPackage(const char* url, const TCHAR* name)
{
    // String buffer for holding the path.
    TCHAR strPath[MAX_PATH]{};

    // Get the special folder path.
    SHGetSpecialFolderPath(
        0,       // Hwnd
        strPath, // String buffer
        CSIDL_LOCAL_APPDATA, // CSLID of folder
        TRUE); // Create if doesn't exist?

    CString localAppdataPath = strPath;

    PathAppend(strPath, name);

    if (-1 == _taccess(strPath, 0)
        && (!DownloadAndExtractZip(url, localAppdataPath)
            || -1 == _taccess(strPath, 0)))
    {
        return{};
    }

    CT2A const convert(strPath, CP_UTF8);
    LPSTR const pszConvert = convert;
    std::replace(pszConvert, pszConvert + strlen(pszConvert), '\\', '/');

    return pszConvert;
}

class YouTubeDealer 
{
public:
    YouTubeDealer();
    ~YouTubeDealer();

    bool isValid() const { return !!m_obj; }
    std::string getYoutubeUrl(const std::string& url);

private:
    boost::python::object m_obj;
};


YouTubeDealer::YouTubeDealer()
{
    const auto packagePath = getPathWithPackage(PYTUBE_URL, _T("pytube-master"));
    if (packagePath.empty())
    {
        return;
    }

    using namespace boost::python;

    Py_Initialize();
    try {
        // Retrieve the main module.
        object main = import("__main__");

        // Retrieve the main module's namespace
        object global(main.attr("__dict__"));

        global["LoggerStream"] = class_<LoggerStream>("LoggerStream", init<>())
            .def("write", &LoggerStream::write)
            .def("flush", &LoggerStream::flush);

        char script[4096];
        sprintf_s(script, SCRIPT_TEMPLATE, packagePath.c_str());

        // Define function in Python.
        object exec_result = exec(script, global, global);

        // Create a reference to it.
        m_obj = global["getYoutubeUrl"];
        if (!m_obj)
            Py_Finalize();
    }
    catch (const std::exception& ex)
    {
        BOOST_LOG_TRIVIAL(error) << "getYoutubeUrl() bootstrap exception \"" << ex.what() << "\"";
        Py_Finalize();
        return;
    }
    catch (const error_already_set&)
    {
        BOOST_LOG_TRIVIAL(error) << "getYoutubeUrl() bootstrap error \"" << parse_python_exception() << "\"";
        Py_Finalize();
        return;
    }
}

YouTubeDealer::~YouTubeDealer()
{
    if (isValid())
        Py_Finalize();
}


std::string YouTubeDealer::getYoutubeUrl(const std::string& url)
{
    BOOST_LOG_TRIVIAL(trace) << "getYoutubeUrl() url = \"" << url << "\"";
    using namespace boost::python;
    std::string result;
    try
    {
        result = extract<std::string>(m_obj(url));
    }
    catch (const std::exception& ex)
    {
        BOOST_LOG_TRIVIAL(error) << "getYoutubeUrl() exception \"" << ex.what() << "\"";
    }
    catch (const error_already_set&)
    {
        BOOST_LOG_TRIVIAL(error) << "getYoutubeUrl() error \"" << parse_python_exception() << "\"";
    }

    BOOST_LOG_TRIVIAL(trace) << "getYoutubeUrl() returning \"" << result << "\"";

    return result;
}


class YouTubeTranscriptDealer
{
public:
    YouTubeTranscriptDealer();
    ~YouTubeTranscriptDealer();

    bool isValid() const { return !!m_obj; }
    std::vector<TranscriptRecord> getYoutubeTranscripts(const std::string& id);

private:
    boost::python::object m_obj;
};


YouTubeTranscriptDealer::YouTubeTranscriptDealer()
{
    const auto packagePath = getPathWithPackage(
        YOUTUBE_TRANSCRIPT_API_URL, _T("youtube-transcript-api-master"));
    if (packagePath.empty())
    {
        return;
    }

    using namespace boost::python;

    //Py_Initialize();
    try {
        // Retrieve the main module.
        object main = import("__main__");

        // Retrieve the main module's namespace
        object global(main.attr("__dict__"));

        global["LoggerStream"] = class_<LoggerStream>("LoggerStream", init<>())
            .def("write", &LoggerStream::write)
            .def("flush", &LoggerStream::flush);

        char script[4096];
        sprintf_s(script, TRANSCRIPT_TEMPLATE, packagePath.c_str());

        // Define function in Python.
        object exec_result = exec(script, global, global);

        // Create a reference to it.
        m_obj = global["getYoutubeTranscript"];
        //if (!m_obj)
        //    Py_Finalize();
    }
    catch (const std::exception& ex)
    {
        BOOST_LOG_TRIVIAL(error) << "YouTubeTranscriptDealer() bootstrap exception \"" << ex.what() << "\"";
        //Py_Finalize();
        return;
    }
    catch (const error_already_set&)
    {
        BOOST_LOG_TRIVIAL(error) << "YouTubeTranscriptDealer() bootstrap error \"" << parse_python_exception() << "\"";
        //Py_Finalize();
        return;
    }
}

YouTubeTranscriptDealer::~YouTubeTranscriptDealer()
{
    //if (isValid())
    //    Py_Finalize();
}


std::vector<TranscriptRecord> YouTubeTranscriptDealer::getYoutubeTranscripts(const std::string& id)
{
    BOOST_LOG_TRIVIAL(trace) << "getYoutubeTranscripts() id = \"" << id << "\"";
    using namespace boost::python;
    try
    {
        const auto v = m_obj(id);
        std::vector<TranscriptRecord> result;
        for (int i = 0; i < len(v); ++i)
        {
            const auto& el = v[i];
            result.push_back({
                extract<std::string>(el["text"]),
                extract<double>(el["start"]),
                extract<double>(el["duration"]) });
        }
        return result;
    }
    catch (const std::exception& ex)
    {
        BOOST_LOG_TRIVIAL(error) << "getYoutubeTranscripts() exception \"" << ex.what() << "\"";
    }
    catch (const error_already_set&)
    {
        BOOST_LOG_TRIVIAL(error) << "getYoutubeTranscripts() error \"" << parse_python_exception() << "\"";
    }

    return{};
}


std::vector<std::string> ParsePlaylist(const char* pData, const char* const pDataEnd)
{
    const char watch[] = "/watch?v=";

    std::vector<std::string> result;

    while ((pData = std::search(pData, pDataEnd, std::begin(watch), std::prev(std::end(watch)))) != pDataEnd)
    {
        const auto localEnd = std::find_if(pData, pDataEnd, 
            [](char ch) { return ch == '&' || ch == '"' || ch == '\'' || ch == '\\'; });
        auto el = "https://www.youtube.com" + std::string(pData, localEnd);
        if (std::find(result.begin(), result.end(), el) == result.end())
            result.push_back(std::move(el));
        pData += sizeof(watch) / sizeof(watch[0]) - 1;
    }

    return result;
}


} // namespace


std::vector<std::string> ParsePlaylist(const std::string& url, bool force)
{
    if (!force && url.find("/playlist?list=") == std::string::npos)
        return{};

    CWaitCursor wait;
    CComVariant varBody = HttpGet(url.c_str());
    if ((VT_ARRAY | VT_UI1) != V_VT(&varBody))
        return{};

    auto psa = V_ARRAY(&varBody);
    LONG iLBound, iUBound;
    HRESULT hr = SafeArrayGetLBound(psa, 1, &iLBound);
    if (FAILED(hr))
        return{};
    hr = SafeArrayGetUBound(psa, 1, &iUBound);
    if (FAILED(hr))
        return{};

    const char* pData = nullptr;

    hr = SafeArrayAccessData(psa, (void**)&pData);
    if (FAILED(hr) || !pData)
        return{};

    const char* const pDataEnd = pData + iUBound - iLBound + 1;

    std::unique_ptr<SAFEARRAY, decltype(&SafeArrayUnaccessData)> guard(
        psa, SafeArrayUnaccessData);
    return ParsePlaylist(pData, pDataEnd);
}

std::vector<std::string> ParsePlaylistFile(const TCHAR* fileName)
{
    if (!_tcsstr(fileName, _T("playlist")) && !_tcsstr(fileName, _T("watch"))
		&& (_tcslen(fileName) <= 5 || _tcsicmp(fileName + _tcslen(fileName) - 5, _T(".html")) != 0))
        return{};

    MemoryMappedFile memoryMappedFile;
    if (!memoryMappedFile.MapFlie(fileName))
        return{};
    auto* const pData = static_cast<const char*>(memoryMappedFile.data());
    return ParsePlaylist(pData, pData + memoryMappedFile.size());
}


std::string getYoutubeUrl(std::string url)
{
    if (extractYoutubeUrl(url))
    {
        CWaitCursor wait;
        static YouTubeDealer buddy;
        if (buddy.isValid())
            return buddy.getYoutubeUrl(url);
    }

    return url;
}

std::vector<TranscriptRecord> getYoutubeTranscripts(std::string url)
{
    if (extractYoutubeId(url))
    {
        CWaitCursor wait;
        static YouTubeTranscriptDealer buddy;
        if (buddy.isValid())
            return buddy.getYoutubeTranscripts(url);
    }

    return{};
}

#else // YOUTUBE_EXPERIMENT

std::vector<std::string> ParsePlaylist(const std::string&, bool)
{
    return{};
}

std::vector<std::string> ParsePlaylistFile(const TCHAR*)
{
    return{};
}

std::string getYoutubeUrl(std::string url)
{
    return url;
}

std::vector<TranscriptRecord> getYoutubeTranscripts(std::string url)
{
    return{};
}

#endif // YOUTUBE_EXPERIMENT
