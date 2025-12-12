# Developers notes

## Design

This is a prototype to play around with the possibility of using the eventlog
for realtime profiling and performance analysis.
There are still numerous open questions:

- Access control?
- Support only Unix domain sockets or also TCP/IP?
- Do we want to support multiple consumers?
- What should happen when a consumer disconnects?
  At the moment, we pause the eventlog stream until a new consumer shows up.
  Alternatively, we could:
  - Close the socket and stop streaming.
  - Pause the program until a new consumer shows up.
  - Kill the program.

## Development

As the most code is C using following line will speedup development
considerably (change your GHC installation path accordingly):

```sh
gcc -c -Iinclude -I/opt/ghc/9.0.1/lib/ghc-9.0.1/include -o eventlog_socket.o cbits/eventlog_socket.c
gcc -c -Iinclude -I/opt/ghc/9.2.0.20210821/lib/ghc-9.2.0.20210821/include -o eventlog_socket.o cbits/eventlog_socket.c
```
