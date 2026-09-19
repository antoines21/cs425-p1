# Project X
- Name: Antoine Sabatier
- Email: antoinesabatier@u.boisestate.edu
- Class: CS425-001

## Known Bugs or Issues

There are no known bugs or issues. The client successfully passes all 21 Unity tests with 100% code coverage. AddressSanitizer and LeakSanitizer report zero memory leaks and zero crashes, including on unexpected server error paths. The application safely handles CRLF injections and dot-stuffing correctly.

## Experience

Building an application layer protocol from scratch based purely on RFC 5321 was a highly educational experience. The biggest initial struggle was managing memory leaks, specifically on the error paths. When simulating bad status codes from the server, the program would return early and skip the `free()` calls. I solved this by implementing a `goto error;` cleanup block, ensuring all dynamically allocated memory is safely freed regardless of when a session fails.

Another interesting challenge was configuring the Unity test framework. I encountered linker errors due to multiple `main` function definitions between my `src/main.c` and the test runner. I resolved this by using a preprocessor macro (`#ifdef TEST`) to rename the application's `main` during test builds, allowing the tests to compile smoothly without duplicating code.

## Design 

To make the SMTP client highly testable without relying on a live mail server, the application is divided into a **three-layer architecture**:

1. **Layer 1: Pure Protocol Helpers**
   This layer contains functions that handle purely string manipulation and protocol rules (e.g., `parse_reply_line`, `build_command`, `build_data_payload`). There is absolutely no network I/O in this layer. It handles the formatting, dot-stuffing, and CRLF injection checks.
   
2. **Layer 2: The Session (Transport-Agnostic)**
   This layer implements the actual sequence of the SMTP protocol (greeting, HELO, MAIL FROM, etc.). Instead of calling `recv` and `send` directly, it interacts with an `io_context` struct that uses function pointers (`read_cb`, `write_cb`). This layer knows *what* to send and expect, but does not know *how* it is physically transmitted.
   
3. **Layer 3: Socket Transport**
   This is the lowest layer, containing the actual networking code (`getaddrinfo`, `connect`, `send`, `recv`). It acts as a thin wrapper that plugs the physical TCP sockets into Layer 2's `io_context`.

**Why this split? (Design for Testability)**
The primary reason for this architecture is **unit testing**. Because Layer 2 operates on abstract function pointers, we can completely swap out Layer 3 during tests. Instead of a real TCP socket, the Unity tests inject a "mocked" in-memory transport layer. This allows the test suite to simulate a complete server session, test chunked or multi-line reads, and trigger specific error paths (like a server hanging up unexpectedly or returning a `550` status code) instantly and reliably, without ever touching a real network interface.
