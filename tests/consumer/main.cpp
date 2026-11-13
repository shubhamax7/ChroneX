#include "chronex/oracle/reference_book.hpp"

int main() {
  chronex::ReferenceBook book;
  const chronex::Command command{
      .sequence = 1,
      .body = chronex::LimitGtc{
          .order_id = 1,
          .side = chronex::Side::Buy,
          .price = 100,
          .quantity = 5,
      },
  };

  const auto events = book.process(command);
  return events.size() == 2 &&
                 events[0].kind == chronex::EventKind::Acknowledged &&
                 events[1].kind == chronex::EventKind::Rested
             ? 0
             : 1;
}
