class LinuxAttachOperation: public AttachOperation {
 private:
  // the connection to the client
  int _socket;
  bool _effectively_completed;

 public:
  void complete(jint res, bufferedStream* st);
  void effectiveley_complete(jint res, bufferedStream* st);
  int get_unix_socket_fd();

  void set_socket(int s)                                { _socket = s; }
  int socket() const                                    { return _socket; }

  LinuxAttachOperation(char* name) : AttachOperation(name) {
    set_socket(-1);
    _effectively_completed = false;
  }
};