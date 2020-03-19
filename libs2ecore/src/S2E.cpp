///
/// Copyright (C) 2010-2016, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2014-2016, Cyberhaven
/// Copyright (C) 2020, TrussLab@University of California, Irvine.
/// 	Authors: Yingtong Liu <yingtong@uci.edu> Hsin-Wei Hung<hsinweih@uci.edu>
/// All rights reserved.
///
/// Licensed under the Cyberhaven Research License Agreement.
///

#include <s2e/cpu.h>

#include <tcg/tcg-llvm.h>

#include <s2e/S2E.h>

#include <s2e/ConfigFile.h>
#include <s2e/CorePlugin.h>
#include <s2e/Plugin.h>
#include <s2e/S2EExecutionState.h>
#include <s2e/S2EExecutor.h>
#include <s2e/Utils.h>

#include <llvm/Config/config.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/raw_ostream.h>
#include <s2e/s2e_libcpu.h>

#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/Module.h>

#include <klee/Common.h>
#include <klee/Interpreter.h>

#include <assert.h>
#include <deque>
#include <errno.h>
#include <iostream>
#include <sstream>
#include <stdlib.h>

#include <stdarg.h>
#include <stdio.h>

#include <ctime>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/labeled_graph.hpp>
#include <boost/graph/topological_sort.hpp>

using namespace boost;

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>
#endif

extern llvm::cl::opt<std::string> PersistentTbCache;

namespace s2e {

using namespace std;

S2E::S2E() {
}

bool S2E::initialize(int argc, char **argv, TCGLLVMContext *tcgLLVMContext, const std::string &configFileName,
                     const std::string &outputDirectory, bool setupUnbufferedStream, int verbose,
                     unsigned s2e_max_processes) {
    m_tcgLLVMContext = tcgLLVMContext;

    if (s2e_max_processes < 1) {
        std::cerr << "You must at least allow one process for S2E." << '\n';
        return false;
    }

    if (s2e_max_processes > S2E_MAX_PROCESSES) {
        std::cerr << "S2E can handle at most " << S2E_MAX_PROCESSES << " processes." << '\n';
        std::cerr << "Please increase the S2E_MAX_PROCESSES constant." << '\n';
        return false;
    }

#ifdef CONFIG_WIN32
    if (s2e_max_processes > 1) {
        std::cerr << "S2E for Windows does not support more than one process" << '\n';
        return false;
    }
#endif

    m_startTimeSeconds = llvm::sys::TimeValue::now().seconds();

    // We are the master process of our group
    setpgid(0, 0);

    m_setupUnbufferedStream = setupUnbufferedStream;
    m_maxProcesses = s2e_max_processes;
    m_currentProcessIndex = 0;
    m_currentProcessId = 0;
    S2EShared *shared = m_sync.acquire();
    shared->currentProcessCount = 1;
    shared->lastStateId = 0;
    shared->lastFileId = 1;
    shared->processIds[m_currentProcessId] = m_currentProcessIndex;
    shared->processPids[m_currentProcessId] = getpid();
    shared->stateModifyingSyscallCalled = false;
    shared->stateRevealingPid = 0;
    shared->waitForReboot = false;
    shared->active = 1;
    m_sync.release();

    /* Open output directory. Do it at the very beginning so that
       other init* functions can use it. */
    initOutputDirectory(outputDirectory, verbose, false);

    /* Copy the config file into the output directory */
    {
        llvm::raw_ostream *out = openOutputFile("s2e.config.lua");
        ifstream in(configFileName.c_str());
        char c;
        while (in.get(c)) {
            (*out) << c;
        }
        delete out;
    }

    /* Save command line arguments */
    {
        llvm::raw_ostream *out = openOutputFile("s2e.cmdline");
        for (int i = 0; i < argc; ++i) {
            if (i != 0)
                (*out) << " ";
            (*out) << "'" << argv[i] << "'";
        }
        delete out;
    }

    /* Parse configuration file */
    m_configFile = new s2e::ConfigFile(configFileName);

    /* Initialize KLEE command line options */
    initKleeOptions();

    /* Initialize S2EExecutor */
    initExecutor();

    initLogging();

    /* Load and initialize plugins */
    initPlugins();

    /* Init the custom memory allocator */
    // void slab_init();
    // slab_init();
    return true;
}

void S2E::writeBitCodeToFile() {
    std::string fileName = getOutputFilename("module.bc");
    if (!PersistentTbCache.empty()) {
        fileName = PersistentTbCache;
    }

    std::error_code error;
    llvm::raw_fd_ostream o(fileName, error, llvm::sys::fs::F_None);

    llvm::Module *module = m_tcgLLVMContext->getModule();

    // Output the bitcode file to stdout
    llvm::WriteBitcodeToFile(module, o);
}

S2E::~S2E() {
    getWarningsStream() << "Terminating node " << m_currentProcessIndex << " (instance slot " << m_currentProcessId
                        << ")\n";

    // Delete all the stuff used by the instance
    m_pluginManager.destroy();

    // Tell other instances we are dead so they can fork more
    S2EShared *shared = m_sync.acquire();

    //FIXME: Mousse 
    //assert(shared->processIds[m_currentProcessId] == m_currentProcessIndex);
    shared->processIds[m_currentProcessId] = (unsigned) -1;
    shared->processPids[m_currentProcessId] = (unsigned) -1;
    assert(shared->currentProcessCount > 0);
    --shared->currentProcessCount;

    m_sync.release();

    writeBitCodeToFile();

    // KModule wants to delete the llvm::Module in destroyer.
    // llvm::ModuleProvider wants to delete it too. We have to arbitrate.

    // Make sure everything is clean
    m_s2eExecutor->flushTb();

    delete m_s2eExecutor;
    delete m_s2eHandler;

    delete m_configFile;

    delete m_warningStream;
    delete m_infoStream;
    delete m_debugStream;

    delete m_infoFileRaw;
    delete m_warningsFileRaw;
    delete m_debugFileRaw;
}

std::string S2E::getOutputFilename(const std::string &fileName) {
    llvm::SmallString<128> filePath(m_outputDirectory);
    llvm::sys::path::append(filePath, fileName);

    return filePath.str();
}

llvm::raw_ostream *S2E::openOutputFile(const std::string &fileName) {
    std::string path = getOutputFilename(fileName);
    std::error_code error;
    llvm::raw_fd_ostream *f = new llvm::raw_fd_ostream(path, error, llvm::sys::fs::F_None);

    if (!f || error) {
        llvm::errs() << "Error opening " << path << ": " << error.message() << "\n";
        exit(-1);
    }

    return f;
}

void S2E::initOutputDirectory(const string &outputDirectory, int verbose, bool forked) {
    if (!forked) {
        // In case we create the first S2E process
        if (outputDirectory.empty()) {
            for (int i = 0;; i++) {
                ostringstream dirName;
                dirName << "s2e-out-" << i;

                llvm::SmallString<128> dirPath(".");
                llvm::sys::path::append(dirPath, dirName.str());

                if (!llvm::sys::fs::exists(dirPath)) {
                    m_outputDirectory = dirPath.str();
                    break;
                }
            }

        } else {
            m_outputDirectory = outputDirectory;
        }
        m_outputDirectoryBase = m_outputDirectory;
    } else {
        m_outputDirectory = m_outputDirectoryBase;
    }

#ifndef _WIN32
    if (m_maxProcesses > 1) {
        // Create one output directory per child process.
        // This prevents child processes from clobbering each other's output.
        llvm::SmallString<128> dirPath(m_outputDirectory);

        ostringstream oss;
        oss << m_currentProcessIndex;

        llvm::sys::path::append(dirPath, oss.str());

        assert(!llvm::sys::fs::exists(dirPath));
        m_outputDirectory = dirPath.str();
    }
#endif

    std::cout << "S2E: output directory = \"" << m_outputDirectory << "\"\n";

    std::error_code mkdirError = llvm::sys::fs::create_directories(m_outputDirectory);
    if (mkdirError) {
        std::cerr << "Could not create output directory " << m_outputDirectory << " error: " << mkdirError.message()
                  << '\n';
        exit(-1);
    }

#ifndef _WIN32
    // Fix directory permissions (createDirectoryOnDisk narrows umask)
    mode_t m = umask(0);
    umask(m);
    chmod(m_outputDirectory.c_str(), 0775 & ~m);

    if (!forked) {
        llvm::SmallString<128> s2eLast(".");
        llvm::sys::path::append(s2eLast, "s2e-last");
        if ((unlink(s2eLast.c_str()) < 0) && (errno != ENOENT)) {
            perror("ERROR: Cannot unlink s2e-last");
            exit(1);
        }

        if (symlink(m_outputDirectoryBase.c_str(), s2eLast.c_str()) < 0) {
            perror("ERROR: Cannot make symlink s2e-last");
            exit(1);
        }
    }
#endif

    setupStreams(forked, true);

    getDebugStream(NULL) << "Revision: " << LIBCPU_REVISION << "\n";
    getDebugStream(NULL) << "Config date: " << CONFIG_DATE << "\n\n";
}

void S2E::setupStreams(bool forked, bool reopen) {
    ios_base::sync_with_stdio(true);
    cout.setf(ios_base::unitbuf);
    cerr.setf(ios_base::unitbuf);

    if (forked) {
        /* Close old file descriptors */
        delete m_infoFileRaw;
        delete m_debugFileRaw;
        delete m_warningsFileRaw;
    }

    if (reopen) {
        m_infoFileRaw = openOutputFile("info.txt");
        m_debugFileRaw = openOutputFile("debug.txt");
        m_warningsFileRaw = openOutputFile("warnings.txt");
    }

    // Debug writes to debug.txt
    raw_tee_ostream *debugStream = new raw_tee_ostream(m_debugFileRaw);

    // Info writes to info.txt and debug.txt
    raw_tee_ostream *infoStream = new raw_tee_ostream(m_infoFileRaw);
    infoStream->addParentBuf(m_debugFileRaw);

    // Warnings appear in debug.txt, warnings.txt and on stderr in red color
    raw_tee_ostream *warningsStream = new raw_tee_ostream(m_warningsFileRaw);
    warningsStream->addParentBuf(m_debugFileRaw);
    warningsStream->addParentBuf(new raw_highlight_ostream(&llvm::errs()));

    // Select which streams also write to the terminal
    switch (m_consoleLevel) {
        case LOG_ALL:
        case LOG_DEBUG:
            debugStream->addParentBuf(&llvm::outs());
        case LOG_INFO:
            infoStream->addParentBuf(&llvm::outs());
        case LOG_WARN:
            /* Warning stream already prints to stderr */
            break;
        case LOG_NONE:
            /* Don't log anything to terminal */
            break;
    }

    m_debugStream = debugStream;
    m_infoStream = infoStream;
    m_warningStream = warningsStream;

    if (m_setupUnbufferedStream) {
        // Make contents valid when assertion fails
        m_infoFileRaw->SetUnbuffered();
        m_infoStream->SetUnbuffered();
        m_debugFileRaw->SetUnbuffered();
        m_debugStream->SetUnbuffered();
        m_warningsFileRaw->SetUnbuffered();
        m_warningStream->SetUnbuffered();
    }

    klee::klee_message_stream = m_infoStream;
    klee::klee_warning_stream = m_warningStream;
}

void S2E::initKleeOptions() {
    std::vector<std::string> kleeOptions = getConfig()->getStringList("s2e.kleeArgs");
    if (!kleeOptions.empty()) {
        int numArgs = kleeOptions.size() + 1;
        const char **kleeArgv = new const char *[numArgs + 1];

        kleeArgv[0] = "s2e.kleeArgs";
        kleeArgv[numArgs] = 0;

        for (unsigned int i = 0; i < kleeOptions.size(); ++i)
            kleeArgv[i + 1] = kleeOptions[i].c_str();

        llvm::cl::ParseCommandLineOptions(numArgs, (char **) kleeArgv);

        delete[] kleeArgv;
    }
}

void S2E::initLogging() {
    bool ok;
    std::string logLevel = getConfig()->getString("s2e.logging.logLevel", "", &ok);

    if (ok) {
        m_hasGlobalLogLevel = true;
        ok = parseLogLevel(logLevel, &m_globalLogLevel);
        if (ok) {
            std::cout << "Using log level override '" << logLevel << "'\n";
        } else {
            std::cerr << "Invalid global log level override '" << logLevel << "'\n";
        }
    }

    std::string consoleOutput = getConfig()->getString("s2e.logging.console", "", &ok);

    if (ok) {
        ok = parseLogLevel(consoleOutput, &m_consoleLevel);
        if (ok) {
            std::cout << "Setting console level to '" << consoleOutput << "'\n";
        } else {
            std::cerr << "Invalid log level '" << consoleOutput << "' for console output, defaulting to '"
                      << DEFAULT_CONSOLE_OUTPUT << "'\n";
            parseLogLevel(DEFAULT_CONSOLE_OUTPUT, &m_consoleLevel);
        }
        /* Setup streams according to configuration. Don't reopen files */
        setupStreams(false, false);
    }
}

void S2E::initPlugins() {
    if (!m_pluginManager.initialize(this, m_configFile)) {
        exit(-1);
    }
}

void S2E::initExecutor() {
    m_s2eHandler = new S2EHandler(this);
    S2EExecutor::InterpreterOptions IOpts;
    m_s2eExecutor = new S2EExecutor(this, m_tcgLLVMContext, IOpts, m_s2eHandler);
}

llvm::raw_ostream &S2E::getStream(llvm::raw_ostream &stream, const S2EExecutionState *state) const {
    fflush(stdout);
    fflush(stderr);

    stream.flush();

    if (state) {
        llvm::sys::TimeValue curTime = llvm::sys::TimeValue::now();
        stream << (curTime.seconds() - m_startTimeSeconds) << ' ';

        if (m_maxProcesses > 1) {
            stream << "[Node " << m_currentProcessIndex << "/" << m_currentProcessId << " - State " << state->getID()
                   << "] ";
        } else {
            stream << "[State " << state->getID() << "] ";
        }
    }
    return stream;
}

void S2E::printf(llvm::raw_ostream &os, const char *fmt, ...) {
    va_list vl;
    va_start(vl, fmt);

    char str[512];
    vsnprintf(str, sizeof(str), fmt, vl);
    os << str;
}

int S2E::fork() {
#if defined(CONFIG_WIN32)
    return -1;
#else

    S2EShared *shared = m_sync.acquire();

    assert(shared->currentProcessCount > 0);
    if (shared->currentProcessCount == m_maxProcesses) {
        m_sync.release();
        return -1;
    }

    unsigned newProcessIndex = shared->lastFileId;
    ++shared->lastFileId;
    ++shared->currentProcessCount;

    ++shared->active;
    m_sync.release();

    pid_t pid = ::fork();
    if (pid < 0) {
        // Fork failed

        shared = m_sync.acquire();

        // Do not decrement lastFileId, as other fork may have
        // succeeded while we were handling the failure.

        assert(shared->currentProcessCount > 1);
        --shared->currentProcessCount;

        m_sync.release();
        return -1;
    }

    if (pid == 0) {
        // Find a free slot in the instance map
    fprintf(stderr, "%s\n",__FUNCTION__);
        shared = m_sync.acquire();
        unsigned i = 0;
        for (i = 0; i < m_maxProcesses; ++i) {
            if (shared->processIds[i] == (unsigned) -1) {
                shared->processIds[i] = newProcessIndex;
                shared->processPids[i] = getpid();
                m_currentProcessId = i;
                break;
            }
        }
        assert(i < m_maxProcesses && "Failed to find a free slot");
        m_sync.release();

        m_currentProcessIndex = newProcessIndex;
        // We are the child process, setup the log files again
        initOutputDirectory(m_outputDirectoryBase, 0, true);

        getWarningsStream() << "Started new node " << newProcessIndex << " (instance slot " << m_currentProcessId
                            << " pid " << getpid() << ")\n";

        // Also recreate new statistics files
        m_s2eExecutor->initializeStatistics();
        // And the solver output
        m_s2eExecutor->initializeSolver();

//        s2e_kvm_clone_process();
    }

    return pid == 0 ? 1 : 0;
#endif
}

unsigned S2E::fetchAndIncrementStateId() {
    S2EShared *shared = m_sync.acquire();
    unsigned ret = shared->lastStateId;
    ++shared->lastStateId;
    m_sync.release();
    return ret;
}
unsigned S2E::fetchNextStateId() {
    S2EShared *shared = m_sync.acquire();
    unsigned ret = shared->lastStateId;
    m_sync.release();
    return ret;
}

unsigned S2E::getCurrentProcessCount() {
    S2EShared *shared = m_sync.acquire();
    unsigned ret = shared->currentProcessCount;
    m_sync.release();
    return ret;
}

unsigned S2E::getProcessIndexForId(unsigned id) {
    assert(id < m_maxProcesses);
    S2EShared *shared = m_sync.acquire();
    unsigned ret = shared->processIds[id];
    m_sync.release();
    return ret;
}

void S2E::setCurrentProcessFinished() {
    S2EShared *shared = m_sync.acquire();
    shared->processIds[m_currentProcessId] = (unsigned) -1;
    shared->processPids[m_currentProcessId] = (unsigned) -1;
    --shared->currentProcessCount;
    m_sync.release();
}

void S2E::setActive(bool active) {
    S2EShared *shared = m_sync.acquire();
    shared->active += active? 1 : -1;
    m_sync.release();
}

bool S2E::getActive() {
    S2EShared *shared = m_sync.acquire();
    bool ret = shared->active;
    m_sync.release();
    return ret;
}

void S2E::setWaitForReboot() {
    S2EShared *shared = m_sync.acquire();
    shared->waitForReboot = true;
    m_sync.release();
}

bool S2E::getWaitForReboot() {
    S2EShared *shared = m_sync.acquire();
    bool ret = shared->waitForReboot;
    m_sync.release();
    return ret;
}

bool S2E::addStateIfClean(uint32_t state_id) {
    S2EShared *shared = m_sync.acquire();
//    shared->stateRevealingMutex.lock();

    uint32_t parent_id = state_id >> 16;
    uint32_t this_id = state_id & 0xffff;
    bool clean = false;
    for (unsigned i = 0; i < S2E_MAX_PROCESSES; ++i) {
        if (shared->cleanStates[i] == parent_id) {
            clean = true;
        }
        if (shared->cleanStates[i] == (unsigned)-1) {
            if (i == 0) { // the set is empty
                shared->cleanStates[i] = this_id;
                clean = true;
            } else if (clean) {
                shared->cleanStates[i] = this_id;
            } else {
                break;
            }
        }
    }

//    shared->stateRevealingMutex.unlock();
    m_sync.release();
    return clean;
}


bool S2E::isStateClean(S2EShared *shared, uint32_t state_id) {
    uint32_t this_id = state_id & 0xffff;
    bool clean = false;
    for (unsigned i = 0; i < S2E_MAX_PROCESSES; ++i) {
        if (shared->cleanStates[i] == this_id) {
            clean = true;
            break;
        }
    }
    return clean;
}

void S2E::setNewCleanState(S2EShared *shared, uint32_t state_id) {
    uint32_t this_id = state_id & 0xffff;
    for (unsigned i = 0; i < S2E_MAX_PROCESSES; ++i)
        shared->cleanStates[i] = (unsigned)-1;
    shared->cleanStates[0] = this_id;
}

void S2E::lockStateRevealingMutex(S2EShared *shared) {
    if (shared->stateRevealingPid == 0) {
        shared->stateRevealingPid = getpid();
        shared->stateRevealingMutex.lock();
        shared->stateRevealingCount++;
    } else if (shared->stateRevealingPid == getpid()) {
        shared->stateRevealingCount++;
    } else {
        shared->stateRevealingMutex.lock();
    }
}

void S2E::unlockStateRevealingMutex(S2EShared *shared) {
    if (shared->stateRevealingPid == getpid()) {
        shared->stateRevealingCount--;
        if (shared->stateRevealingCount == 0) {
            shared->stateRevealingPid = 0;
            shared->stateRevealingMutex.unlock();
        }
    }
}

bool S2E::callStateModifyingIfClean(uint32_t state_id) {
    S2EShared *shared = m_sync.acquire();

    bool clean = isStateClean(shared, state_id);
    if (clean) {
        lockStateRevealingMutex(shared);
        setNewCleanState(shared, state_id);
    }

    unlockStateRevealingMutex(shared);
    m_sync.release();
    return clean;
}

bool S2E::callStateRevealingStartIfClean(uint32_t state_id) {
    S2EShared *shared = m_sync.acquire();

    bool clean = isStateClean(shared, state_id);
    if (clean)
        lockStateRevealingMutex(shared);

    m_sync.release();
    return clean;
}

void S2E::callStateRevealingEnd() {
    S2EShared *shared = m_sync.acquire();
    unlockStateRevealingMutex(shared);
    m_sync.release();
}

int S2E::connectToServer(int port) {
    S2EShared *shared = m_sync.acquire();
    struct sockaddr_in serv_addr;

    if ((shared->serverSocketFD = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        getWarningsStream() << "failed to create socket\n";
        m_sync.release();
        return -1;
    }

    memset(&serv_addr, '0', sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        getWarningsStream() << "invalid address/Address not supported\n";
        m_sync.release();
        return -1;
    }

    if (connect(shared->serverSocketFD, (struct sockaddr *) &serv_addr,
                sizeof(serv_addr)) < 0) {
        getWarningsStream() << "Connection failed\n";
        m_sync.release();
        return -1;
    }

    /* FIXME: close the sock at the end of execution */
    //close(m_sock);
    m_sync.release();
    return 0;
}

int S2E::getDataFromServer(uint32_t opcode, void *data, int size) {
    S2EShared *shared = m_sync.acquire();
    int respSize;
    uint32_t resp;

    uint32_t opc = opcode;
    send(shared->serverSocketFD, &opc, 4, 0);

    respSize = read(shared->serverSocketFD, &resp, 4);
    if (respSize != 4) {
        getWarningsStream() << "failed to read the response type\n";
        close(shared->serverSocketFD);
        m_sync.release();
        return -1;
    }
    if (resp != 0) {
        time_t now = time(0);
        char* dt = ctime(&now);
        getWarningsStream() << "server has no data available at: " << dt << "\n";
        m_sync.release();
        return -2;
    }

    respSize = read(shared->serverSocketFD, data, size);
    if (respSize != size) {
        getWarningsStream() << "failed to read the data\n";
        close(shared->serverSocketFD);
        m_sync.release();
        return -1;
    }

    m_sync.release();
    return 0;
}

void S2E::sendRawDataToServer(void *data, int size) {
    S2EShared *shared = m_sync.acquire();
    send(shared->serverSocketFD, data, size, 0);
    m_sync.release();
}

void S2E::sendDataToServer(uint32_t opcode, void *data, int size) {
    S2EShared *shared = m_sync.acquire();
    uint32_t opc = opcode;
    send(shared->serverSocketFD, &opc, 4, 0);
    send(shared->serverSocketFD, data, size, 0);
    m_sync.release();
}

int S2E::sendQueryToServer(uint32_t opcode, uint64_t query, void *rdata, int size) {
    S2EShared *shared = m_sync.acquire();
    int respSize;
    uint32_t opc = opcode;
    send(shared->serverSocketFD, &opc, 4, 0);
    send(shared->serverSocketFD, &query, 8, 0);
    respSize = read(shared->serverSocketFD, rdata, size);
    if (respSize != size) {
        getWarningsStream() << "failed to read the response type\n";
        close(shared->serverSocketFD);
        return -1;
    }

    m_sync.release();
    return 0;
}

} // namespace s2e

/******************************/

extern "C" {

s2e::S2E *g_s2e = NULL;

void *get_s2e(void) {
    return g_s2e;
}

void s2e_initialize(int argc, char **argv, TCGLLVMContext *tcgLLVMContext, const char *s2e_config_file,
                    const char *s2e_output_dir, int setup_unbuffered_stream, int verbose, unsigned s2e_max_processes) {
    g_s2e = new s2e::S2E();
    if (!g_s2e->initialize(argc, argv, tcgLLVMContext, s2e_config_file ? s2e_config_file : "",
                           s2e_output_dir ? s2e_output_dir : "", setup_unbuffered_stream, verbose, s2e_max_processes)) {
        exit(-1);
    }
}

void s2e_close(void) {
    delete g_s2e;
    tcg_llvm_close(tcg_llvm_ctx);
    tcg_llvm_ctx = NULL;
    g_s2e = NULL;
}

void s2e_flush_output_streams(void) {
    g_s2e->flushOutputStreams();
}

int s2e_vprintf(const char *fmtstr, int warn, va_list args) {
    if (!g_s2e) {
        return 0;
    }

    char str[4096];
    int ret = vsnprintf(str, sizeof(str), fmtstr, args);

    if (warn) {
        g_s2e->getWarningsStream() << str;
    } else {
        g_s2e->getDebugStream() << str;
    }

    return ret;
}

void s2e_debug_print(const char *fmtstr, ...) {
    va_list vl;
    va_start(vl, fmtstr);
    s2e_vprintf(fmtstr, 0, vl);
    va_end(vl);
}

void s2e_warning_print(const char *fmtstr, ...) {
    va_list vl;
    va_start(vl, fmtstr);
    s2e_vprintf(fmtstr, 1, vl);
    va_end(vl);
}

void s2e_debug_print_hex(void *addr, int len) {
    int i;
    unsigned char buff[17];
    unsigned char *pc = (unsigned char *) addr;
    char tempbuff[512] = {0};
    char line[512] = {0};

    // Process every byte in the data.
    for (i = 0; i < len; i++) {
        // Multiple of 16 means new line (with line offset).

        if ((i % 16) == 0) {
            // Just don't print ASCII for the zeroth line.
            if (i != 0) {
                sprintf(tempbuff, "  %s\n", buff);
                strcat(line, tempbuff);
                g_s2e->getDebugStream() << line;
                line[0] = 0;
            }

            // Output the offset.
            sprintf(tempbuff, "  %04x ", i);
            strcat(line, tempbuff);
        }

        // Now the hex code for the specific character.
        sprintf(tempbuff, " %02x", pc[i]);
        strcat(line, tempbuff);

        // And store a printable ASCII character for later.
        if ((pc[i] < 0x20) || (pc[i] > 0x7e))
            buff[i % 16] = '.';
        else
            buff[i % 16] = pc[i];
        buff[(i % 16) + 1] = '\0';
    }

    // Pad out last line if not exactly 16 characters.
    while ((i % 16) != 0) {
        sprintf(tempbuff, "   ");
        strcat(line, tempbuff);
        i++;
    }

    // And print the final ASCII bit.
    sprintf(tempbuff, "  %s\n", buff);
    strcat(line, tempbuff);
    g_s2e->getDebugStream() << line;
}

void s2e_print_constraints(void);
void s2e_print_constraints(void) {
    g_s2e->getDebugStream() << "===== Constraints =====\n";
    for (auto c : g_s2e_state->constraints) {
        g_s2e->getDebugStream() << c << '\n';
    }
    g_s2e->getDebugStream() << "\n";
}

// Print a klee expression.
// Useful for invocations from GDB
void s2e_print_expr(void *expr);
void s2e_print_expr(void *expr) {
    klee::ref<klee::Expr> e = *(klee::ref<klee::Expr> *) expr;
    std::stringstream ss;
    ss << e;
    g_s2e->getDebugStream() << ss.str() << '\n';
}

void s2e_print_value(void *value);
void s2e_print_value(void *value) {
    llvm::Value *v = (llvm::Value *) value;
    g_s2e->getDebugStream() << *v << '\n';
}

extern "C" {
void s2e_execute_cmd(const char *cmd) {
    g_s2e->getConfig()->invokeLuaCommand(cmd);
}

// Non-S2E modules can redeclare this variable with __attribute__((weak))
// to check whether they run in S2E or not.
int g_s2e_linked = 1;
}

} // extern "C"
