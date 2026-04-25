#pragma once
#include <chrono>
#include <concepts>
#include <functional>

namespace dxmt {

using CommandListClock = std::chrono::high_resolution_clock;

struct CommandListExecutionProfile {
  unsigned command_count = 0;
  unsigned slow_command_count = 0;
  unsigned max_command_index = 0;
  CommandListClock::duration max_command_duration {};
  const char *max_command_name = nullptr;
};

template <typename F, typename context>
concept CommandWithContext = requires(F f, context &ctx) {
  { f(ctx) } -> std::same_as<void>;
};

namespace impl {
template <typename context> class CommandBase {
public:
  virtual void invoke(context &) = 0;
  virtual const char *name() const noexcept = 0;
  virtual ~CommandBase() noexcept {};
  CommandBase<context> *next = nullptr;
};

template <typename context, typename F> class LambdaCommand final : public CommandBase<context> {
public:
  void
  invoke(context &ctx) final {
    std::invoke(func, ctx);
  };
  const char *
  name() const noexcept final {
    return __PRETTY_FUNCTION__;
  }
  ~LambdaCommand() noexcept final = default;
  LambdaCommand(F &&ff) : CommandBase<context>(), func(std::forward<F>(ff)) {}
  LambdaCommand(const LambdaCommand &copy) = delete;
  LambdaCommand &operator=(const LambdaCommand &copy_assign) = delete;

private:
  F func;
};

template <typename context> class EmptyCommand final : public CommandBase<context> {
public:
  void invoke(context &ctx) final{/* nop */};
  const char *name() const noexcept final { return "EmptyCommand"; };
  ~EmptyCommand() noexcept = default;
};

template <typename value_t> struct LinkedListNode {
  value_t value;
  LinkedListNode *next;
};

} // namespace impl

template <typename Context> class CommandList {

  impl::EmptyCommand<Context> empty;
  impl::CommandBase<Context> *list_end;

public:
  CommandList() : list_end(&empty) {
    empty.next = nullptr;
  }
  ~CommandList() {
    reset();
  }

  void
  reset() {
    impl::CommandBase<Context> *cur = empty.next;
    while (cur) {
      auto next = cur->next;
      cur->~CommandBase<Context>(); // call destructor
      cur = next;
    }
    empty.next = nullptr;
    list_end = &empty;
  }

  CommandList(const CommandList &copy) = delete;
  CommandList(CommandList &&move) {
    this->reset();
    empty.next = move.empty.next;
    list_end = move.list_end;
    move.empty.next = nullptr;
    move.list_end = nullptr;
  }

  CommandList& operator=(CommandList&& move) {
    this->reset();
    empty.next = move.empty.next;
    list_end = move.list_end;
    move.empty.next = nullptr;
    move.list_end = nullptr;
    return *this;
  }

  template <CommandWithContext<Context> Fn>
  constexpr unsigned
  calculateCommandSize() {
    using command_t = impl::LambdaCommand<Context, Fn>;
    return sizeof(command_t);
  }

  template <CommandWithContext<Context> Fn>
  unsigned
  emit(Fn &&cmd, void *buffer) {
    using command_t = impl::LambdaCommand<Context, Fn>;
    auto cmd_h = new (buffer) command_t(std::forward<Fn>(cmd));
    list_end->next = cmd_h;
    list_end = cmd_h;
    return sizeof(command_t);
  }

  void append(CommandList &&list) {
    list_end->next = list.empty.next;
    list_end = list.list_end;
    list.empty.next = nullptr;
    list.list_end = nullptr;
  }

  CommandListExecutionProfile
  execute(Context &context, bool profile = false) {
    CommandListExecutionProfile result;
    impl::CommandBase<Context> *cur = empty.next;
    while (cur) {
      if (profile) {
        auto t0 = CommandListClock::now();
        cur->invoke(context);
        auto elapsed = CommandListClock::now() - t0;
        if (elapsed > result.max_command_duration) {
          result.max_command_duration = elapsed;
          result.max_command_index = result.command_count;
          result.max_command_name = cur->name();
        }
        if (std::chrono::duration<double, std::milli>(elapsed).count() > 1.0)
          result.slow_command_count++;
      } else {
        cur->invoke(context);
      }
      result.command_count++;
      cur = cur->next;
    }
    return result;
  };
};

}; // namespace dxmt
