#pragma once

class MO {
 public:
  void begin();
  void loop();

  [[nodiscard]] bool isStarted() const noexcept;

 private:
  bool started_{false};
};
