// Copyright 2020 Your Name <your_email>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/config.hpp>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "Json_storage.hpp"
#include "Suggestions_collection.hpp"

namespace beast = boost::beast;    // from <boost/beast.hpp>
namespace http = beast::http;      // from <boost/beast/http.hpp>
namespace net = boost::asio;       // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;  // from <boost/asio/ip/tcp.hpp>
std::string make_json(const json& data) {
  std::stringstream ss;
  if (data.is_null())
    ss << "No suggestions";
  else
    ss << std::setw(4) << data;
  return ss.str();
}
// Реализация взята с
// https://github.com/boostorg/beast/blob/develop/example/http/server/sync/http_server_sync.cpp

// Эта функция производит HTTP-ответ для данного запроса.
// Тип объекта ответа зависит от содержимого запроса,
// поэтому интерфейс требует, чтобы вызывающий объект
// передал универсальную лямбду для получения ответа.

template <class Body, class Allocator, class Send>
void handle_request(http::request<Body, http::basic_fields<Allocator>>&& req,
                    Send&& send, const std::shared_ptr<std::timed_mutex>& mutex,
                    const std::shared_ptr<Suggestions_collection>& collection) {
  // Возвращает ответ типа "плохой" на запрос

  auto const bad_request = [&req](beast::string_view why) {
    http::response<http::string_body> res{http::status::bad_request,
                                          req.version()};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req.keep_alive());
    res.body() = std::string(why);
    res.prepare_payload();
    return res;
  };

  // Возвращает ответ типа "ненайденный" на запрос

  auto const not_found = [&req](beast::string_view target) {
    http::response<http::string_body> res{http::status::not_found,
                                          req.version()};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req.keep_alive());
    res.body() = "The resource '" + std::string(target) + "' was not found.";
    res.prepare_payload();
    return res;
  };

  // Проверка правильности типа запроса он должен быть только POST!!!

  // Первый запрос GET обернем его красиво :)
  if (req.method() == http::verb::get) {
    return send(bad_request("Hello!"));
  }

  // Убеждаемся, что это POST-запрос
  if (req.method() != http::verb::post && req.method() != http::verb::head) {
    return send(bad_request("Unknown HTTP-method"));
  }

  // Убеждаемся, что адресс корректный
  if (req.target() != "/v1/api/suggest") {
    return send(not_found(req.target()));
  }

  // Парсим данные из тела запроса
  json input_body;
  try {
    input_body = json::parse(req.body());
  } catch (std::exception& e) {
    return send(bad_request(e.what()));
  }
  boost::optional<std::string> input;
  try {
    input = input_body.at("input").get<std::string>();
  } catch (std::exception& e) {
    return send(bad_request(R"(JSON format: {"input": "<user_input>"}!)"));
  }
  if (!input.has_value()) {
    return send(bad_request(R"(JSON format: {"input": "<user_input>"}!)"));
  }

  // Формируем тело ответа - suggestion

  // READ - блокировка
  mutex->lock();
  auto result = collection->Suggest(input.value());
  mutex->unlock();
  http::string_body::value_type body = make_json(result);
  auto const size = body.size();

  // Ответ на HEAD-запрос
  if (req.method() == http::verb::head) {
    http::response<http::empty_body> res{http::status::ok, req.version()};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "application/json");
    res.content_length(size);
    res.keep_alive(req.keep_alive());
    return send(std::move(res));
  }

  // Отправка suggestions обратно пользователю
  http::response<http::string_body> res{
      std::piecewise_construct, std::make_tuple(std::move(body)),
      std::make_tuple(http::status::ok, req.version())};
  res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
  res.set(http::field::content_type, "application/json");
  res.content_length(size);
  res.keep_alive(req.keep_alive());
  return send(std::move(res));
}

// Сообщить о сбое
void fail(beast::error_code ec, char const* what) {
  std::cerr << what << ": " << ec.message() << "\n";
}

// Это эквивалент C++11 универсальной лямбды.
// Объект функции используется для отправки HTTP-сообщения.
template <class Stream>
struct send_lambda {
  Stream& stream_;
  [[maybe_unused]] bool& close_;
  beast::error_code& ec_;

  explicit send_lambda(Stream& stream, bool& close, beast::error_code& ec)
      : stream_(stream), close_(close), ec_(ec) {}

  template <bool isRequest, class Body, class Fields>
  void operator()(http::message<isRequest, Body, Fields>&& msg) const {
    // Определите, следует ли нам закрыть соединение после этого.
    close_ = msg.need_eof();

    // Нам нужен сериализатор здесь, потому что сериализатор требует
    // неконстантное тело файла и ориентированная на сообщение версия
    // http::write работает только с сообщениями const.
    http::serializer<isRequest, Body, Fields> sr{msg};
    http::write(stream_, sr, ec_);
  }
};

// Обрабатывает соединение с HTTP - сервером
// Принимает входящий TCP-сокет
void do_session(net::ip::tcp::socket& socket,
                const std::shared_ptr<Suggestions_collection>& collection,
                const std::shared_ptr<std::timed_mutex>& mutex) {
  bool close = false;
  beast::error_code ec;

  // Этот буфер необходим для сохранения во время чтения
  beast::flat_buffer buffer;

  // Эта лямбда используется для отправки сообщений
  send_lambda<tcp::socket> lambda{socket, close, ec};
  for (;;) {
    // Чтение запроса
    http::request<http::string_body> req;
    http::read(socket, buffer, req, ec);
    if (ec == http::error::end_of_stream) break;
    if (ec) return fail(ec, "read");

    // Отправка ответа
    handle_request(std::move(req), lambda, mutex, collection);
    if (ec) return fail(ec, "write");
  }

  // Отправка TCP-сокета завершающего соединение
  socket.shutdown(tcp::socket::shutdown_send, ec);
  // В этот момент соединение корректно закрывается
}

// Обновление коллекции suggestions происходит каждые 15 минут из того же файла
void suggestion_updater(
    const std::shared_ptr<Json_storage>& storage,
    const std::shared_ptr<Suggestions_collection>& suggestions,
    const std::shared_ptr<std::timed_mutex>& mutex) {
  using std::chrono_literals::operator""min;
  for (;;) {
    // WRITE - блокировка
    mutex->lock();
    storage->Load();
    suggestions->Update(storage->get_storage());
    mutex->unlock();
    std::cout << "Suggestions updated" << std::endl;
    std::this_thread::sleep_for(15min);
  }
}
int Run_server(int argc, char* argv[]) {
  // Создание умных указателей на объекты, используемых для обращения к файлу и
  // его обновлению
  std::shared_ptr<std::timed_mutex> mutex =
      std::make_shared<std::timed_mutex>();
  std::shared_ptr<Json_storage> storage = std::make_shared<Json_storage>(
      "/Users/evgenii/CLionProjects/lab-07-http-server/suggestions.json");
  std::shared_ptr<Suggestions_collection> suggestions =
      std::make_shared<Suggestions_collection>();
  try {
    // Проверка аргументов командной строки
    if (argc != 3) {
      std::cerr << "Usage: suggestion_server <address> <port>\n"
                << "Example:\n"
                << "    http-server-sync 0.0.0.0 8080\n";
      return EXIT_FAILURE;
    }
    auto const address = net::ip::make_address(argv[1]);
    auto const port = static_cast<uint16_t>(std::atoi(argv[2]));

    // io_context требуется для всех операций ввода-вывода
    net::io_context ioc{1};

    // Акцептор принимает входящие соединения
    tcp::acceptor acceptor{ioc, {address, port}};

    // Выделяем поток, который будет обновлять коллекцию
    // и отсоединяем его, так как он не будет возвращать данные для работы
    std::thread{suggestion_updater, storage, suggestions, mutex}.detach();
    for (;;) {
      // Создаем новое соединение
      tcp::socket socket{ioc};

      // Блокируем, пока мы не получим соединение
      acceptor.accept(socket);

      // Запустите сеанс, передав право собственности на сокет
      std::thread{std::bind(&do_session, std::move(socket), suggestions, mutex)}
          .detach();
    }
  } catch (std::exception& e) {
    std::cerr << e.what() << '\n';
    return EXIT_FAILURE;
  }
}
// Using: ./cmake-build-debug/tests 0.0.0.0 8080
// int main(int argc, char* argv[]) {
//  return Run_server(argc, argv);
//}
