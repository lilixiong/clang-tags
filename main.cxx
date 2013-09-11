#include "config.h"

#include "util/util.hxx"
#include "request/request.hxx"
#include "getopt++/getopt.hxx"
#include "MT/stream.hxx"

#include "clangTags/storage/sqliteDB.hxx"

#include "clangTags/update/thread.hxx"
#include "clangTags/watch/inotify.hxx"

#include "clangTags/load.hxx"
#include "clangTags/config.hxx"
#include "clangTags/findDefinition.hxx"
#include "clangTags/complete.hxx"
#include "clangTags/grep.hxx"

#include <boost/asio.hpp>
#include <boost/thread/thread.hpp>

namespace ClangTags {

class LoadCommand : public Request::CommandParser {
public:
  LoadCommand (const std::string & name,
               Storage::Interface & storage,
               Update::Thread & update)
    : Request::CommandParser (name, "Read a compilation database"),
      loader_ (storage, update)
  {
    prompt_ = "load> ";
    defaults();

    using Request::key;
    add (key ("database", args_.fileName)
         ->metavar ("FILEPATH")
         ->description ("Load compilation commands from a JSON compilation database"));
  }

  void defaults () {
    args_.fileName = "compile_commands.json";
  }

  void run (std::ostream & cout) {
    loader_ (args_, cout);
  }

private:
  Load loader_;
  Load::Args args_;
};

class ConfigCommand : public Request::CommandParser {
public:
  ConfigCommand (const std::string & name,
                 Storage::Interface & storage)
    : Request::CommandParser (name, "Get/set clang-tags configuration"),
      config_ (storage)
  {
    prompt_ = "config> ";
    defaults();

    using Request::key;
    add (key ("get", args_.get)
         ->metavar ("true|false")
         ->description ("if true, get the option value. Otherwise, set it"));
    add (key ("option", args_.name)
         ->metavar ("NAME")
         ->description ("option name"));
    add (key ("value", args_.value)
         ->metavar ("JSON_VAL")
         ->description ("JSON-encoded option value"));
  }

  void defaults () {
    args_.get = false;
  }

  void run (std::ostream & cout) {
    config_ (args_, cout);
  }

private:
  Config config_;
  Config::Args args_;
};

class IndexCommand : public Request::CommandParser {
public:
  IndexCommand (const std::string & name,
                Update::Thread & update)
    : Request::CommandParser (name, "Update the source code index"),
      update_ (update)
  {
    prompt_ = "index> ";
  }

  void run (std::ostream & cout) {
    update_.index();
    cout << "Waiting for the index to be rebuilt..." << std::endl;
    update_.wait();
    cout << "Done." << std::endl;
  }

protected:
  Update::Thread & update_;
};


class FindCommand : public Request::CommandParser {
public:
  FindCommand (const std::string & name,
               Storage::Interface & storage,
               Cache & cache)
    : Request::CommandParser (name, "Find the definition of a symbol"),
      findDefinition_ (storage, cache)
  {
    prompt_ = "find> ";
    defaults ();

    using Request::key;
    add (key ("file", args_.fileName)
         ->metavar ("FILENAME")
         ->description ("Source file name"));
    add (key ("offset", args_.offset)
         ->metavar ("OFFSET")
         ->description ("Offset in bytes"));
    add (key ("mostSpecific", args_.mostSpecific)
         ->metavar ("true|false")
         ->description ("Display only the most specific identifier at this location"));
    add (key ("diagnostics", args_.diagnostics)
         ->metavar ("true|false")
         ->description ("Print compilation diagnostics"));
    add (key ("fromIndex", args_.fromIndex)
         ->metavar ("true|false")
         ->description ("Search in the index (faster but potentially out-of-date)"));
  }

  void defaults () {
    args_.fileName = "";
    args_.offset = 0;
    args_.mostSpecific = false;
    args_.diagnostics = true;
    args_.fromIndex = true;
  }

  void run (std::ostream & cout) {
    findDefinition_ (args_, cout);
  }

private:
  FindDefinition findDefinition_;
  FindDefinition::Args args_;
};


class GrepCommand : public Request::CommandParser {
public:
  GrepCommand (const std::string & name,
               Storage::Interface & storage)
    : Request::CommandParser (name, "Find all references to a definition"),
      grep_ (storage)
  {
    prompt_ = "grep> ";
    defaults();

    using Request::key;
    add (key ("usr", args_.usr)
         ->metavar ("USR")
         ->description ("Unified Symbol Resolution for the symbol"));
  }

  void defaults () {
    args_.usr = "c:@F@main";
  }

  void run (std::ostream & cout) {
    grep_ (args_, cout);
  }

private:
  Grep grep_;
  Grep::Args args_;
};


class CompleteCommand : public Request::CommandParser {
public:
  CompleteCommand (const std::string & name,
                   Storage::Interface & storage,
                   Cache & cache)
    : Request::CommandParser (name, "Complete the code at point"),
      complete_ (storage, cache)
  {
    prompt_ = "complete> ";
    defaults();

    using Request::key;
    add (key ("file", args_.fileName)
         ->metavar ("FILENAME")
         ->description ("Source file name"));
    add (key ("line", args_.line)
         ->metavar ("LINE_NO")
         ->description ("Line number (counting from 0)"));
    add (key ("column", args_.column)
         ->metavar ("COLUMN_NO")
         ->description ("Column number (counting from 0)"));
  }

  void defaults () {
    args_.fileName = "";
    args_.line = 0;
    args_.column = 0;
  }

  void run (std::ostream & cout) {
    complete_ (args_, cout);
  }

private:
  Complete complete_;
  Complete::Args args_;
};

struct ExitCommand : public Request::CommandParser {
  ExitCommand (const std::string & name)
    : Request::CommandParser (name, "Shutdown server")
  {
    prompt_ = "exit> ";
  }

  void run (std::ostream & cout) {
    cout << "Exiting..." << std::endl;
    throw std::runtime_error ("shutdown requested");
  }
};
}

class Serve {
public:
  Serve (Request::Parser & parser)
    : parser_     (parser),
      pidPath_    (".ct.pid"),
      socketPath_ (".ct.sock")
  {
    std::ofstream pidFile (pidPath_);
    pidFile << getpid() << std::endl;
    pidFile.close();
  }

  ~Serve () {
    MT::cerr() << "Server exiting..." << std::endl;
    unlink (socketPath_.c_str());
    unlink (pidPath_.c_str());
  }

  void operator() () {
    MT::cerr() << "Server starting with pid: " << getpid() << std::endl;

    boost::asio::io_service io_service;
    boost::asio::local::stream_protocol::endpoint endpoint (socketPath_);
    boost::asio::local::stream_protocol::acceptor acceptor (io_service, endpoint);
    for (;;) {
      boost::asio::local::stream_protocol::iostream socket;
      boost::system::error_code err;
      acceptor.accept(*socket.rdbuf(), err);
      if (!err) {
        parser_.parseJson (socket, socket, /*verbose=*/true);
      }
    }
  }

private:
  Request::Parser & parser_;
  std::string pidPath_;
  std::string socketPath_;
};

int main (int argc, char **argv) {
  Getopt options (argc, argv);
  options.add ("help", 'h', 0,
               "print this help message and exit");
  options.add ("stdin", 's', 0,
               "read a request from the standard input and exit");

  try {
    options.get();
  } catch (...) {
    std::cerr << options.usage();
    return 1;
  }

  if (options.getCount ("help") > 0) {
    std::cerr << options.usage();
    return 0;
  }

  try {
    ClangTags::Cache cache;
    ClangTags::Storage::SqliteDB storage;

    std::list<boost::thread> threads;

    ClangTags::Update::Thread update (cache);
    threads.push_back (boost::thread (boost::ref(update)));

#if defined(HAVE_INOTIFY)
    ClangTags::Watch::Inotify watch (update);
    update.setWatchThread (&watch);
    threads.push_back (boost::thread (boost::ref(watch)));
#endif

    Request::Parser p ("Clang-tags server\n");
    p .add (new ClangTags::LoadCommand   ("load",   storage, update))
      .add (new ClangTags::ConfigCommand ("config", storage))
      .add (new ClangTags::IndexCommand  ("index",  update))
      .add (new ClangTags::FindCommand   ("find",   storage, cache))
      .add (new ClangTags::GrepCommand   ("grep",   storage))
      .add (new ClangTags::CompleteCommand ("complete", storage, cache))
      .add (new ClangTags::ExitCommand   ("exit"))
      .prompt ("clang-dde> ");

    if (options.getCount ("stdin") > 0) {
      p.parseJson (std::cin, std::cout);
    }
    else {
      try {
        Serve serve (p);
        serve ();
      } catch (...) {}
    }

    for (auto it = threads.begin() ; it != threads.end() ; ++it)
      it->interrupt();
    for (auto it = threads.begin() ; it != threads.end() ; ++it)
      it->join();
  }
  catch (std::exception& e) {
    std::cerr << std::endl << "Caught exception: " << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

