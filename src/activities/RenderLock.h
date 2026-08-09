#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  // Tag selecting the non-blocking constructor.
  struct TryToLock {};

  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  // Non-blocking: takes the lock if it is free and leaves it unheld otherwise. Check isHeld()
  // before touching anything the lock protects. Used where blocking would stall the main loop for
  // the length of a render, which is where the buttons are sampled.
  explicit RenderLock(TryToLock);
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  bool isHeld() const { return isLocked; }
  // Wait for the lock, for a caller that tried non-blocking first and cannot proceed without it.
  // No-op when already held.
  void lockBlocking();
  void unlock();
  static bool peek();
};
