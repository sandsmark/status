#pragma once

// C
#include <string.h>

// C++
#include <memory>
#include <string>
#include <vector>

// external
#include <pulse/pulseaudio.h>

using std::string;
using std::vector;
using std::unique_ptr;

class Sink {
 public:
  typedef enum {
    AVAILABLE_UNKNOWN = 0,
    AVAILABLE_NO,
    AVAILABLE_YES,
  } Availability;

  Sink(const pa_sink_info* info);

  uint32_t Index() const { return index_; }
  const string& Name() const { return name_; }
  int Volume() const { return volume_percent_; }
  bool Muted() const { return mute_; }

 private:
  friend class PulseClient;

  uint32_t index_;
  string name_;
  string desc_;
  int volume_percent_;
  int mute_;
  Sink::Availability available_ = Sink::AVAILABLE_UNKNOWN;
};

struct ServerInfo {
  string sink;
};

class PulseClient {
 public:
  PulseClient(string client_name);
  ~PulseClient();

  // Populates all known devices and cards. Any currently known
  // devices and cards are cleared before the new data is stored.
  void Populate();

  // Get the default sink
  const Sink *GetDefaultSink() const;

 private:
  bool init();
  bool wait_for_op(pa_operation* op);

  bool populate_server_info();
  bool populate_sinks();

  string client_name_;
  pa_context* context_;
  pa_mainloop* mainloop_;
  vector<Sink> sinks_;
  std::string default_sink_;
  pa_context_state state_;
};

// vim: set et ts=2 sw=2:
