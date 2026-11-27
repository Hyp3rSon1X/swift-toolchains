#include "Copy.hpp"
#include "refcxx.h"
#include <memory>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

namespace {
class Shape {
public:
  virtual ~Shape() = default;
  virtual double area() const = 0;
  virtual std::string name() const = 0;
};

class Rect : public Shape {
public:
  Rect(double w, double h) : w(w), h(h) {}
  double area() const override { return w * h; }
  std::string name() const override { return "rect"; }

protected:
  double w, h;
};

class Square final : public Rect {
public:
  explicit Square(double s) : Rect(s, s) {}
  std::string name() const override { return "square"; }
};

class Circle final : public Shape {
public:
  explicit Circle(double r) : r(r) {}
  double area() const override { return 3.0 * r * r; }
  std::string name() const override { return "circle"; }

private:
  double r;
};
} // namespace

extern "C" int refcxx_shapes(char *buf, int cap) {
  std::vector<std::shared_ptr<Shape>> shapes{std::make_shared<Rect>(2, 3), std::make_shared<Square>(4),
                                             std::make_shared<Circle>(1)};
  std::shared_ptr<Shape> extra = shapes[1];
  int rects = 0, squares = 0;
  double total = 0;
  for (const auto &s : shapes) {
    total += s->area();
    if (dynamic_cast<Rect *>(s.get()))
      ++rects;
    if (dynamic_cast<Square *>(s.get()))
      ++squares;
  }
  std::ostringstream os;
  os << "shapes " << shapes.size() << " area " << total << " rects " << rects << " squares " << squares
     << " typeid-match " << (typeid(*shapes.back()) == typeid(Circle)) << " use_count " << extra.use_count();
  for (const auto &s : shapes)
    os << ' ' << s->name();
  return refcxx::copyOut(os.str(), buf, cap);
}
