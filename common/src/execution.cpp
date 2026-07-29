#include <lob/execution.hpp>

namespace lob {

std::string_view OrderStateName(OrderState s) {
  switch (s) {
    case OrderState::kPendingNew:
      return "PENDING_NEW";
    case OrderState::kResting:
      return "RESTING";
    case OrderState::kPartiallyFilled:
      return "PARTIALLY_FILLED";
    case OrderState::kFilled:
      return "FILLED";
    case OrderState::kPendingCancel:
      return "PENDING_CANCEL";
    case OrderState::kCanceled:
      return "CANCELED";
    case OrderState::kRejected:
      return "REJECTED";
  }
  return "?";
}

std::string_view FillCauseName(FillCause c) {
  switch (c) {
    case FillCause::kQueueConsumed:
      return "queue_consumed";
    case FillCause::kTradedThrough:
      return "traded_through";
    case FillCause::kCrossed:
      return "crossed";
  }
  return "?";
}

}  // namespace lob
