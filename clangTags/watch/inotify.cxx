#include "inotify.hxx"

#include "MT/stream.hxx"

#include <sys/inotify.h>
#include <sys/poll.h>

namespace ClangTags {
namespace Watch {

Inotify::Inotify (Update::Thread & updateThread)
  : updateRequested_ (true),
    updateThread_ (updateThread)
{
  // Initialize inotify
  fd_inotify_ = inotify_init ();
  if (fd_inotify_ == -1) {
    perror ("inotify_init");
    throw std::runtime_error ("inotify");
  }
}

Inotify::~Inotify () {
  // Close inotify file
  // -> the kernel will clean up all associated watches
  ::close (fd_inotify_);
}

void Inotify::update () {
  updateRequested_.set (true);
}

void Inotify::update_ () {
  MT::cerr() << "Updating watchlist..." << std::endl;
  std::vector<std::string> list = storage_.listFiles();
  for (auto it=list.begin() ; it!=list.end() ; ++it) {
    std::string fileName = *it;

    // Skip already watched files
    if (inotifyMap_.contains(fileName)) {
      continue;
    }

    MT::cerr() << "Watching " << fileName << std::endl;
    int wd = inotify_add_watch (fd_inotify_, fileName.c_str(), IN_MODIFY);
    if (wd == -1) {
      perror ("inotify_add_watch");
    }

    inotifyMap_.add (fileName, wd);
  }
}

void Inotify::operator() () {
  const size_t BUF_LEN = 1024;
  char buf[BUF_LEN] __attribute__((aligned(4)));

  struct pollfd fd;
  fd.fd = fd_inotify_;
  fd.events = POLLIN;

  for ( ; ; ) {
    // Check for interruptions
    boost::this_thread::interruption_point();

    // Check whether a re-indexing was requested
    if (updateRequested_.get() == true) {
      // Reindex and update list
      update_();

      // Reset flag and notify waiting threads
      updateRequested_.set (false);
    }

    // Look for an inotify event
    switch (poll (&fd, 1, 1000)) {
    case -1:
      perror ("poll");
      break;

    default:
      if (fd.revents & POLLIN) {
        ssize_t len, i = 0;
        len = read (fd.fd, buf, BUF_LEN);
        if (len < 1) {
          perror ("read");
          break;
        }

        while (i<len) {
          // Read event & update index
          struct inotify_event *event = (struct inotify_event *) &(buf[i]);
          i += sizeof (struct inotify_event) + event->len;

          MT::cerr() << "Detected modification of "
                    << inotifyMap_.fileName(event->wd) << std::endl;
        }

        // Schedule an index update
        updateThread_.index();
      }
    }
  }
}
}
}
