# Job System
A small, fast, work-stealing job system written in C++ 20.

Inspired by Naughty Dog's job system described in Jason Gregory's book Game Engine Architecture.

## Design Features
- Zero heap allocation on hot path, every job is exactly one cache line (64 bytes), allocated from a per-thread flat ring buffer.
- Lock free scheduling, each worker owns work-stealing deque. The owner pushes/pops from bottom; thieves steal from the top.
- Parent/Child dependencies, a job isn't "finished" until all of its children are finished.
- Helping while waiting, thread isn't blocked, it keeps stealing and executing other jobs until the one it is waiting on completes.