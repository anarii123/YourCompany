#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "ui_dialog.h"
#include "ui_formed.h"
#include <QPainter>
#include <QInputDialog>
#include <QDebug>
#include <QMessageBox>
#include <QDateTime>
#include <QThread>
#include <QMouseEvent>

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/thread.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/asio.hpp>
#include <boost/asio/deadline_timer.hpp>

#ifdef WIN32
#include <mstcpip.h>
#undef min
#undef errno
#undef error
#endif
#define TCP_KEEPALIVE_SECS 5

enum  {
    cmd_type_auth = 0,
    cmd_type_auth_ok,
    cmd_type_formed,
    cmd_type_get_usr_list,
    cmd_type_formed_ok,
    cmd_type_get_contract,
    cmd_type_get_contract_ok,
    cmd_type_finish_market,
    cmd_type_auction,
    cmd_type_amount,
    cmd_type_auction_win,
    cmd_type_auction_lose,
    cmd_type_report,
    cmd_type_err,
};

std::string base64_encode(const std::string &s)
{
    static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i=0,ix=0,leng = s.length();
    std::stringstream q;

    for(i=0,ix=leng - leng%3; i<ix; i+=3)
    {
        q<< base64_chars[ (s[i] & 0xfc) >> 2 ];
        q<< base64_chars[ ((s[i] & 0x03) << 4) + ((s[i+1] & 0xf0) >> 4)  ];
        q<< base64_chars[ ((s[i+1] & 0x0f) << 2) + ((s[i+2] & 0xc0) >> 6)  ];
        q<< base64_chars[ s[i+2] & 0x3f ];
    }
    if (ix<leng)
    {
        q<< base64_chars[ (s[ix] & 0xfc) >> 2 ];
        q<< base64_chars[ ((s[ix] & 0x03) << 4) + (ix+1<leng ? (s[ix+1] & 0xf0) >> 4 : 0)];
        q<< (ix+1<leng ? base64_chars[ ((s[ix+1] & 0x0f) << 2) ] : '=');
        q<< '=';
    }
    return q.str();
}

class Connector
{
    boost::mutex start_stop_mtx;
     boost::shared_ptr<boost::asio::io_service> io_service;
     boost::shared_ptr<boost::asio::io_service::work> ios_work;
     boost::shared_ptr<boost::thread> worker_thread;
    boost::shared_ptr<boost::asio::ip::tcp::resolver> resolver;
    boost::shared_ptr<boost::asio::ip::tcp::socket> socket;

    std::string host;
    uint16_t port;
    std::string login;
    std::string password;
    uint64_t reconnect_if_no_response;
    std::vector<uint8_t> read_buffer;
    std::vector<uint8_t> parse_buffer;

    GUIUpdater* data_receiver;

    struct ToSend {
        std::vector<uint8_t> data;
    };

public:
    Connector(GUIUpdater* data_receiver)
        : reconnect_if_no_response(0)
        , data_receiver(data_receiver)
    {
        read_buffer.resize(2048);
    }

    ~Connector()
    {
        stop();
    }

    void start(const std::string& host_, uint16_t port_, const std::string& login_, const std::string& password_)
    {
        boost::mutex::scoped_lock lock(start_stop_mtx);
        assert(!io_service && !ios_work && !worker_thread && !resolver);
        host = host_;
        port = port_;
        login = login_;
        password = password_;

        reconnect_if_no_response = QDateTime::currentMSecsSinceEpoch();

        io_service.reset(new boost::asio::io_service);
        ios_work.reset(new boost::asio::io_service::work(*io_service));
        resolver.reset(new boost::asio::ip::tcp::resolver(*io_service));
        worker_thread.reset(new boost::thread(boost::bind(&boost::asio::io_service::run, io_service)));
        boost::shared_ptr<boost::asio::deadline_timer> keep_alive_timer(
                    new boost::asio::deadline_timer(*io_service)
                    );
        keep_alive_timer->expires_from_now(boost::posix_time::seconds(1));
        keep_alive_timer->async_wait(boost::bind(&Connector::keep_alive, this, keep_alive_timer));
    }

    void stop()
    {
        reconnect_if_no_response = 0;

        {
            boost::mutex::scoped_lock lock(start_stop_mtx);
            ios_work.reset();
            if (io_service)
                io_service->stop();
            if (worker_thread) {
                worker_thread->join();
                worker_thread.reset();
            }
            if (socket) {
                socket->close();
                socket.reset();
            }
            resolver.reset();
            io_service.reset();
        }
    }

    void send_command_new_state(int type, bool enabled, int id_paradox, int id)
    {
//        std::string data = stdprintf("%d:%d:%d:%d", type, enabled, id_paradox, id);
//        command_send( cmd_type_set_new_state, reinterpret_cast<uint8_t*>(&data[0]), data.size());
    }


public:

    void command_send(uint8_t cmd, uint8_t* cmd_data, uint32_t size)
    {
        boost::shared_ptr<ToSend> to_send(new ToSend);
        to_send->data.resize(size + 7);
        size_t it = 0;
        uint8_t* data = &to_send->data[0];
        data[it++] = 13;
        data[it++] = 37;
        memcpy(&data[it], &size, 4); it += 4;
        data[it++] = cmd;

        assert(it + size == to_send->data.size());
        if (size)
            memcpy(&to_send->data[it], cmd_data, size);

        boost::asio::async_write(*socket,
            boost::asio::buffer(reinterpret_cast<const char*>(&to_send->data[0]), to_send->data.size()),
            boost::asio::transfer_all(),
            boost::bind(
                &Connector::on_send_over,
                this,
                to_send,
                boost::asio::placeholders::error
                )
            );
    }

    void on_send_over(
        boost::shared_ptr<ToSend>& /*to_send*/,
        const boost::system::error_code& error
        )
    {
        if (error) {
//            log_timestamp("Paradox: error data sending to %s\n", host.c_str());
            reconnect_if_no_response = QDateTime::currentMSecsSinceEpoch();
            data_receiver->system_state_update(STATE_DISCONNECTED, true);
        }
    }

    void read_data()
    {
        boost::asio::async_read(
                *socket,
                boost::asio::buffer(&read_buffer[0], read_buffer.size()),
                boost::asio::transfer_at_least(1),
                boost::bind(
                    &Connector::handle_read,
                    this,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred
                    )
        );
    }

    bool buffer_parse(uint8_t* packet, size_t len)
    {
        parse_buffer.insert(parse_buffer.end(), packet, &packet[len]);
        while (!parse_buffer.empty()) {
            uint8_t* data = &parse_buffer[0];
            size_t size = parse_buffer.size();
            if (size < 7)
                return true;
            if (data[0] != 13 || data[1] != 37) {
//                log_warning("Paradox: wrong data format\n");
                return false;
            }

            uint32_t data_len;
            memcpy(&data_len, &data[2], 4);
            if (data_len + 7 > size)
                return true;

            uint8_t cmd = data[6];

            switch (cmd) {
            case cmd_type_auth_ok: {
//                std::string usr_list(reinterpret_cast<const char*>(&data[7]), data_len);
                data_receiver->system_state_update(STATE_CONNECTED, true);
//                data_receiver->show_usr_list(usr_list);
                reconnect_if_no_response = 0;
////                command_send(cmd_type_get_tree, 0, 0);
                break;
////            case cmd_type_tree:
////                tree_reply_parse(&data[7], data_len);
////                break;
////            case cmd_type_state_upd: {
////                uint8_t level;
////                int sz = 0;
////                boost::shared_ptr<ParadoxElement> element = paradox_element_deserialize(
////                    &data[7],
////                    data_len,
////                    level,
////                    sz
////                    );
////                if (element)
////                    data_receiver->object_changed(level, element);
////                break; }
            }
            case cmd_type_get_usr_list:
            {
                 std::string usr_list(reinterpret_cast<const char*>(&data[7]), data_len);
                 data_receiver->show_usr_list(usr_list);
                break;
            }
            case cmd_type_formed_ok:
            {
                 data_receiver->form_closed();
                break;
            }

            case cmd_type_get_contract_ok:
            {
                std::string contract_info(reinterpret_cast<const char*>(&data[7]), data_len);
                data_receiver->update_contract_info(contract_info);
                break;

            }
            case cmd_type_finish_market:
            {
                std::string contract_info(reinterpret_cast<const char*>(&data[7]), data_len);
                data_receiver->update_contract_info(contract_info);
                break;
            }
            case cmd_type_err: {
                std::string err_msg(reinterpret_cast<const char*>(&data[7]), data_len);
                if (err_msg == "Unauthorized") {
                    data_receiver->system_state_update(
                        STATE_INVALID_LOGIN,
                        true
                        );

                    parse_buffer.erase(parse_buffer.begin(), parse_buffer.begin() + 7 + static_cast<int>(data_len));
                    reconnect_if_no_response = 0;
                    return false;
                }
                break;
            }
////            case cmd_type_send_troubles_info:
////            {
////                std::string event(reinterpret_cast<const char*>(&data[7]), data_len);
////                data_receiver->send_trouble_event(event);
////                break;
//            }
            default:
////                log_warning("Paradox: unsupported cmd 0x%02x received\n", cmd);
                break;
            }
            parse_buffer.erase(parse_buffer.begin(), parse_buffer.begin() + 7 + static_cast<int>(data_len));
        }
        return true;
    }

    void handle_read(const boost::system::error_code &error, size_t bytes_transfered)
    {
        if (error || !bytes_transfered || !buffer_parse(&read_buffer[0], bytes_transfered)) {
//            log_timestamp("Paradox: error read data (size %lu) from %s\n", bytes_transfered, host.c_str());
            reconnect_if_no_response = QDateTime::currentMSecsSinceEpoch();
            data_receiver->system_state_update(STATE_DISCONNECTED, false);
            return;
        };
        read_data();
    }

    void connect_cb(const boost::system::error_code& error)
    {
        if (error) {
//            log_timestamp("Paradox: can't connect to %s\n", host.c_str());
            reconnect_if_no_response = QDateTime::currentMSecsSinceEpoch();
            data_receiver->system_state_update(STATE_DISCONNECTED, true);
            return;
        }
        //data_receiver->system_state_update(STATE_CONNECTED);
        std::string auth_data = base64_encode(login + "@" + password);
        command_send(cmd_type_auth, reinterpret_cast<uint8_t*>(&auth_data[0]), auth_data.size());
        read_data();

#ifdef WIN32
        DWORD ret_bytes = 0;
        struct tcp_keepalive keepalive_opts;
        keepalive_opts.onoff = TRUE;
        keepalive_opts.keepalivetime = TCP_KEEPALIVE_SECS * 1000;
        keepalive_opts.keepaliveinterval = TCP_KEEPALIVE_SECS * 1000;
        int res = WSAIoctl(
                    socket->native_handle(),
                    SIO_KEEPALIVE_VALS,
                    &keepalive_opts,
                    sizeof(keepalive_opts),
                    NULL,
                    0,
                    &ret_bytes,
                    NULL,
                    NULL);
        if (res == SOCKET_ERROR) {
//            log_warning("ASIO: WSAIotcl(SIO_KEEPALIVE_VALS) failed (%d)\n", WSAGetLastError());
        }
#else
        int optval = 1;
        if (setsockopt(socket->native_handle(), SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) == -1) {
            log_warning("ASIO: setsockopt(SO_KEEPALIVE) failed (%s)\n", strerror(errno));
            return;
        }
        optval = 3;
        if (setsockopt(socket->native_handle(), SOL_TCP, TCP_KEEPCNT, &optval, sizeof(optval)) == -1) {
            log_warning("ASIO: setsockopt(TCP_KEEPCNT) failed (%s)\n", strerror(errno));
            return;
        }
        optval = TCP_KEEPALIVE_SECS;
        if (setsockopt(socket->native_handle(), SOL_TCP, TCP_KEEPIDLE, &optval, sizeof(optval)) == -1) {
            log_warning("ASIO: setsockopt(TCP_KEEPIDLE) failed (%s)\n", strerror(errno));
            return;
        }
        optval = TCP_KEEPALIVE_SECS;
        if (setsockopt(socket->native_handle(), SOL_TCP, TCP_KEEPINTVL, &optval, sizeof(optval)) == -1) {
            log_warning("ASIO: setsockopt(TCP_KEEPINTVL) failed (%s)\n", strerror(errno));
            return;
        }
#endif
    }

    void resolve_cb(const boost::system::error_code& error, boost::asio::ip::tcp::resolver::iterator i)
    {
        if (error) {
//            log_timestamp("Paradox: unable to resolve %s\n", host.c_str());
            reconnect_if_no_response = QDateTime::currentMSecsSinceEpoch();
            data_receiver->system_state_update(STATE_DISCONNECTED, true);
            return;
        }
        if (socket)
            socket->close();
        socket.reset(new boost::asio::ip::tcp::socket(*io_service));
        socket->async_connect(*i, boost::bind(&Connector::connect_cb, this, boost::asio::placeholders::error));

#ifndef WIN32
        try {
            boost::asio::ip::tcp::no_delay delay_option(true);
            socket->set_option(delay_option);

            boost::asio::socket_base::keep_alive keep_alive_option(true);
            socket->set_option(keep_alive_option);
        } catch (const std::exception& e) {
            log_timestamp("Paradox: connection to %s setup error: %s\n", host.c_str(), e.what());
        }
#endif
    }

    void connect()
    {
        assert(resolver);
        parse_buffer.clear();

        data_receiver->system_state_update(STATE_DISCONNECTED, false);
        boost::asio::ip::tcp::resolver::query query(host, /*stdprintf("%hu", port)*/ "5000");
        resolver->async_resolve(query, boost::bind(&Connector::resolve_cb,
            this,
            boost::asio::placeholders::error,
            boost::asio::placeholders::iterator));
        reconnect_if_no_response = QDateTime::currentMSecsSinceEpoch() + 10000000ULL;

    }

    void keep_alive(boost::shared_ptr<boost::asio::deadline_timer>& keep_alive_timer)
    {
        keep_alive_timer->expires_from_now(boost::posix_time::seconds(reconnect_if_no_response ? 10 : 1));
        if (reconnect_if_no_response && reconnect_if_no_response < QDateTime::currentMSecsSinceEpoch()) {
//            log_timestamp(
//                "Paradox: create new connection with %s:%hu %s\n",
//                host.c_str(),
//                port,
//                reconnect_if_no_response ? "" : "because of timeout"
//                );
            reconnect_if_no_response = QDateTime::currentMSecsSinceEpoch() + 10000000ULL;
            connect();
        }
        keep_alive_timer->async_wait(boost::bind(&Connector::keep_alive, this, keep_alive_timer));
    }
};

Connector *connector = NULL;


MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    gui_state(IDLE_STATE),
    money(50),
    credit_max_time(42),
    Office(NULL),
    WarehouseMaterials(NULL),
    WarehouseProduct(NULL),
    BuyMaterials(NULL),
    SaleProducts(NULL),
    materials(0),
    connect_status(false),
    form(parent, this),
    dialog(parent, this),
    count_year(4)
{
    ui->setupUi(this);
    //ui->BuyMaterials->hide();
    ui->NewCredit->hide();
    ui->BuyNewProductLine->hide();
    ui->Market->hide();
    memset(debit, 0, 4 * sizeof(int));
    updater = new GUIUpdater();
    connector = new Connector(updater);
    QThread *thread = new QThread;

    connect(thread, SIGNAL(started()), updater, SLOT(newLabel()));
    connect(updater, SIGNAL(requestNewLabel(int)), this, SLOT(process(int)));
    connect(updater, SIGNAL(requestNewUpdateInfo(QString)), this, SLOT(update_contract_info(QString)));
    connect(updater, SIGNAL(requestChangeUsers(QString)), this, SLOT(show_change_users(QString)));
    connect(updater, SIGNAL(requestFormClosed()), this, SLOT(form_closed()));
    updater->moveToThread(thread);
    thread->start();
    markets.push_back(Market("A", QRect(270, 50, 250, 250), Qt::red, &money));
    markets.push_back(Market("B", QRect(530, 50, 250, 250), Qt::blue, &money));
    markets.push_back(Market("C", QRect(790, 50, 250, 250), Qt::yellow, &money, 2, 2));
    markets.push_back(Market("CENTRAL", QRect(10, 50, 250, 250), Qt::gray, &money, 0, 0));
}

MainWindow::~MainWindow()
{
    delete Office;
    delete BuyMaterials;
    delete SaleProducts;
    delete WarehouseProduct;
    delete WarehouseMaterials;
    delete ui;
    if (!ProductLines.empty()) {
           for (std::list<ProductLine*>::iterator it = ProductLines.begin();
                it != ProductLines.end(); ++it)
               delete *it;
    }
    //thread->stop();
    //delete updater;
}

void MainWindow::create_circles(QWidget *group_box, QPainter& p, int finish_count, bool have_credit = false)
{
    int j = 0; int count = 0;
    for (int i = 0; i < finish_count; ++i) {
        if (group_box->x() + 20 + count * 30 + 20 > group_box->x() + group_box->width()) {
             count = 0;
             ++j;
        }
        if (have_credit) {
            for (std::size_t index = 0; index < CreditLines.size(); ++index) {
                if (CreditLines[index].time == finish_count - i - 1)
                        p.setBrush(QBrush(Qt::blue));
            }
        }
        p.drawEllipse(group_box->x() + 10 + count * 30, group_box->y() + 60 + j* 30, 20, 20);
        if (have_credit)
             p.setBrush(Qt::NoBrush);
        ++count;
    }
}

void MainWindow::create_circles_with_name()
{
    QPainter p(this);
    for (std::size_t i = 0; i < markets.size(); ++i) {
        p.setPen(QPen(markets[i].selected ? Qt::green : markets[i].color, markets[i].selected ? 5 : 1,Qt::SolidLine));
        p.setBrush(QBrush(markets[i].color));
        p.drawEllipse(markets[i].rect);

        QPoint point;
        point.setX(markets[i].rect.x() + markets[i].rect.width() /2 - (markets[i].name.size() == 1 ? 10 : 30 * (int) (markets[i].name.size() / 2)));
        point.setY(markets[i].rect.y()  + markets[i].rect.height() /2);
        p.setPen(QPen(Qt::black,1,Qt::SolidLine));
        p.setBrush(QBrush(Qt::black));
        p.setFont(QFont("Arial", 30));
        p.drawText(point, tr(markets[i].name.c_str()));
        point.setX(markets[i].rect.x() + markets[i].rect.width() /2 - 30);
        point.setY(markets[i].rect.y() + 40 + markets[i].rect.height() /2);
        p.setFont(QFont("Arial", 15));
        p.drawText(point, markets[i].is_opened() ? tr("Opened") : tr("Closed"));
        p.setPen(QPen(Qt::blue, 3,Qt::SolidLine));
        p.setBrush(Qt::NoBrush);
        if (i == index_current_market) {
            QRect rect = markets[i].rect;
            rect.setX(rect.x() - 3);
            rect.setY(rect.y() - 3);
            rect.setWidth(rect.width() + 4);
            rect.setHeight(rect.height() + 4);
            p.drawRect(rect);
        }
    }
}

void MainWindow::create_triangles(QWidget *group_box, QPainter& p, void* finish_count)
{
    int j = 0,  count = 0;
    enum { TypeMaterials, TypeProduct } type;
    if (group_box->objectName() == "WarehouseProduct")
        type = TypeProduct;
    else
            type = TypeMaterials;

    for (int i = 0; i < (type == TypeMaterials ? *(reinterpret_cast<int*> (finish_count)) :
                            (reinterpret_cast<std::vector<Product>* >(finish_count))->size()); ++i) {
        if (group_box->x() + 20 + count * 30 + 20 > group_box->x() + group_box->width()) {
             count = 0;
             ++j;
        }
        QVector<QPoint> vec;
        vec.push_back(QPoint(group_box->x() + 10 + count * 30, group_box->y() + 80 + j* 30));
        vec.push_back(QPoint(group_box->x() + 10 + count * 30 + 10, group_box->y() + 60 + j* 30));
        vec.push_back(QPoint(group_box->x() + 10 + count * 30 + 20, group_box->y() + 80 + j* 30));
        QPolygon polygon(vec);
        if (type == TypeProduct) {
              Product product = reinterpret_cast<std::vector<Product>* >(finish_count)->operator [](i);
                if (product.type == Product::TypeA) {
                    p.setPen(QPen(Qt::green,1,Qt::SolidLine));
                    p.setBrush(QBrush(Qt::green));
                } else {
                    p.setPen(QPen(Qt::blue,1,Qt::SolidLine));
                    p.setBrush(QBrush(Qt::blue));
                }
        }
        p.drawPolygon(polygon);
        ++count;
    }
}

void MainWindow::create_square(QWidget *group_box, QPainter& p, int finish_count)
{
    int j = 0,  count = 0;
    for (int i = 0; i < finish_count; ++i) {
        if (group_box->x() + 20 + count * 30 + 20 > group_box->x() + group_box->width()) {
             count = 0;
             ++j;
        }
        QVector<QPoint> vec;
        vec.push_back(QPoint(group_box->x() + 10 + count * 30, group_box->y() + 60 + j* 30));
        vec.push_back(QPoint(group_box->x() + 10 + count * 30 + 20, group_box->y() + 60 + j* 30));
        vec.push_back(QPoint(group_box->x() + 10 + count * 30 + 20, group_box->y() + 80 + j* 30));
        vec.push_back(QPoint(group_box->x() + 10 + count * 30, group_box->y() + 80 + j* 30));
        QPolygon polygon(vec);
        p.drawPolygon(polygon);
        ++count;
    }
}

void MainWindow::createStausBar()
{
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::set_new_group_box(QWidget*& widget, QWidget*& parent, const std::string& name, QRect rect)
{
    widget = new QGroupBox(parent);
    widget->setGeometry(rect);
    widget->setObjectName(QString(name.c_str()));
    (reinterpret_cast<QGroupBox*> (widget))->setTitle(QString(name.c_str()));
    widget->show();
}

void MainWindow::set_new_button(QWidget *&widget, QWidget *&parent, QRect rect, ProductLine* product_line)
{
      widget = new QPushButton(parent);
      widget->setGeometry(rect);
      widget->setObjectName(">>");
      (reinterpret_cast<QPushButton*> (widget))->setText(">>");
      widget->show();
      connect(widget, SIGNAL(clicked(bool)), product_line, SLOT(DownloadMaterials_clicked()));

}

void MainWindow::paintEvent(QPaintEvent *)
{
    switch (gui_state) {
    case MAIN_STATE:
    {
       QPainter p(this);
       p.setPen(QPen(Qt::red,1,Qt::SolidLine));
       p.setBrush(QBrush(Qt::red));
       create_circles(ui->Money, p, money);
       p.setBrush(Qt::NoBrush);
       p.setPen(QPen(Qt::blue,1,Qt::SolidLine));
       create_circles(ui->CreditPayment, p, credit_max_time, true);
       if (!Office)
          set_new_group_box(Office, ui->centralWidget, "Office", QRect(270, 10, 441, 51));
       else
           (reinterpret_cast<QGroupBox*> (Office))->show();
       if (!WarehouseMaterials) {
          set_new_group_box(WarehouseMaterials, ui->centralWidget, "WarehouseMaterials", QRect(20, 10, 241, 271));
          if (!BuyMaterials) {
              BuyMaterials = new QPushButton(WarehouseMaterials);
              BuyMaterials->setObjectName("BuyMaterials");
              BuyMaterials->setGeometry(QRect(150, 240, 80, 21));
              ((QPushButton*) BuyMaterials)->setText("BuyMaterials");
              connect(BuyMaterials, SIGNAL(clicked(bool)), this, SLOT(BuyMaterials_clicked()));
              BuyMaterials->show();
          }
       } else {
           (reinterpret_cast<QGroupBox*> (WarehouseMaterials))->show();
       }
       if (!WarehouseProduct) {
           set_new_group_box(WarehouseProduct, ui->centralWidget, "WarehouseProduct", QRect(730, 10, 291, 191));
           if (!SaleProducts) {
               SaleProducts = new QPushButton(WarehouseProduct);
               SaleProducts->setObjectName("SaleProducts");
               SaleProducts->setGeometry(QRect(170, 140, 80, 21));
               ((QPushButton*) SaleProducts)->setText("SaleProducts");
               connect(SaleProducts, SIGNAL(clicked(bool)), this, SLOT(SaleProducts_clicked()));
               SaleProducts->show();
           }
       } else {
            (reinterpret_cast<QGroupBox*> (WarehouseProduct))->show();
       }
        if (!ProductLines.empty()) {
           std::string pr_line_str = ProductLines.back()->product_type.toStdString();
            if (!(ProductLines.back()->widget)) {
                QRect rect = new_location_product_line();
                set_new_group_box(ProductLines.back()->widget, ui->centralWidget,  std::string("Line")
                                  + std::to_string(ProductLines.size() - 1) + pr_line_str, rect);
                rect.setX(rect.x() - 25);
                rect.setWidth(20);
                set_new_button(ProductLines.back()->button, ui->centralWidget, rect, ProductLines.back());
            }
            for (std::list<ProductLine*>::iterator it = ProductLines.begin(); it != ProductLines.end(); ++it) {
                p.setPen(QPen(Qt::blue,1,Qt::SolidLine));
                (reinterpret_cast<QGroupBox*> ((*it)->widget))->show();
                (reinterpret_cast<QPushButton*> ((*it)->button))->show();
                (*it)->create_empty_triangles_in_product_line(p);
            }
        }
        if (materials) {
            p.setPen(QPen(Qt::yellow,1,Qt::SolidLine));
            p.setBrush(QBrush(Qt::yellow));
            create_triangles(WarehouseMaterials, p, (void*) &materials);
        }
        if (!products.empty()) {
            p.setPen(QPen(Qt::blue,1,Qt::SolidLine));
            p.setBrush(QBrush(Qt::blue));
            create_triangles(WarehouseProduct, p, (void*) &products);
        }
        p.setPen(QPen(Qt::red,1,Qt::SolidLine));
        p.setBrush(QBrush(Qt::red));
        create_square(ui->Debit1, p, debit[0]);
        create_square(ui->Debit2, p, debit[1]);
        create_square(ui->Debit3, p, debit[2]);
        create_square(ui->Debit4, p, debit[3]);
        ui->CreditPayment->show();
        ui->Debit1->show();
        ui->Debit2->show();
        ui->Debit3->show();
        ui->Debit4->show();
        ui->NewCredit->show();
        break;
    }
    case MARKET_STATE:
    {
        (reinterpret_cast<QGroupBox*> (WarehouseProduct))->hide();
        (reinterpret_cast<QGroupBox*> (WarehouseMaterials))->hide();
        (reinterpret_cast<QGroupBox*> (Office))->hide();
        ui->CreditPayment->hide();
        ui->Debit1->hide();
        ui->Debit2->hide();
        ui->Debit3->hide();
        ui->Debit4->hide();
        ui->NewCredit->hide();
        QPainter p(this);
        p.setPen(QPen(Qt::red,1,Qt::SolidLine));
        p.setBrush(QBrush(Qt::red));
        for (std::list<ProductLine*>::iterator it = ProductLines.begin(); it != ProductLines.end(); ++it) {
            (reinterpret_cast<QGroupBox*> ((*it)->widget))->hide();
            (reinterpret_cast<QPushButton*> ((*it)->button))->hide();
        }
        create_circles(ui->Money, p, money);
        create_circles_with_name();
    }
    };
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    for (std::size_t i = 0; i < markets.size(); ++i) {
        int xc, yc, r, x, y;
        xc = markets[i].rect.x() + markets[i].rect.width() / 2;
        yc = markets[i].rect.y() + markets[i].rect.height() / 2;
        r = markets[i].rect.width() / 2;
        x = event->x();
        y = event->y();
        if (((x - xc) * (x - xc) + (y - yc) * (y - yc) <= r * r)
                && !markets[i].selected
                && money > 0) {
               markets[i].selected = true;
               markets[i].recount_before();
        }
    }
    repaint();
}

void MainWindow::on_Start_clicked()
{
    // Code for connect to server
    if (gui_state == IDLE_STATE) {
        createStausBar();
        dialog.show();
    } else {
        for (std::size_t index = 0; index < CreditLines.size(); ++index) {
            CreditLines[index].time--;
        }
        for (std::list<ProductLine*>::iterator it = ProductLines.begin(); it != ProductLines.end(); ++it) {
            (*it)->MatPerTime.recount();
            (*it)->button->setEnabled(true);
        }
        if (debit[3] > 0)
            money += debit[3];
        for (int i = 3; i > 0; --i) {
            debit[i] = debit[i - 1];
            debit[i - 1] = 0;
        }
        --count_year;
        if (!count_year) {
            for (std::size_t i = 0; i < markets.size(); ++i) {
                markets[i].recount_after();
                markets[i].recount_before();
            }
            count_year = 4;
        }
    }
    repaint();

}

QRect MainWindow::new_location_product_line()
{
   int dx = 0 , dy =0;
    qDebug() << ProductLines.size() << "\n";
   for (std::size_t index_line = 1; index_line < ProductLines.size(); ++index_line) {
       if (dx + (220 + 210 + 270) < 720)
         dx += 220;
       else {
         dx  -= 220;
         dy += 140;
       }

   }
    QRect rect(300 + dx, 61 + dy, 180, 130);
    return rect;
}

void MainWindow::add_materials()
{
    bool ok = false;
    int count_materials = QInputDialog::getInt(this, tr("Type Product Materials"),
                                         tr("Type Product Materials (input A or B):"), 0, 0, 1000, 1, &ok);
    if (ok && (count_materials)) {
        if (money > 0) {
            money -= 2 * count_materials;
            materials += count_materials;
            repaint();
        } else {
            QMessageBox msgBox;
            msgBox.setText("No money");
            msgBox.setStandardButtons(QMessageBox::Ok);
            msgBox.exec();
        }
    }

}

void MainWindow::sale_products()
{
//    bool ok = false;
//    QString materials = QInputDialog::getText(this, tr("Sale Product"),
//                                         tr("What products do you want to sale (FOR EXAMPLE 10A2B) ?"), QLineEdit::Normal,
//                                                "", &ok);
//    QRegExp rx("(\\d+)[A|a](\\d+)[B|b]");
//    if (rx.indexIn(materials) != -1) {
//        int a = rx.cap(1).toInt();
//        int b = rx.cap(2).toInt();
        int count_a = 0, count_b = 0;
        for (std::size_t i = 0; i < products.size() ; ++i) {
            if (products[i].type == Product::TypeA)
                ++count_a;
            else
                ++count_b;
        }
//        if (count_a < a || count_b < b) {
//            QMessageBox msgBox;
//            msgBox.setText("No products");
//            msgBox.setStandardButtons(QMessageBox::Ok);
//            msgBox.exec();
//        } else {
            for (std::size_t j = 0; j < contracts.size();) {
                if (contracts[j].a <= count_a && contracts[j].b <= count_b) {
                    for (std::size_t i = 0; i < products.size() ;) {
                        if (products[i].type == Product::TypeA && contracts[j].a > 0) {
                            products.erase(products.begin() + i);
                            std::vector<Product>(products).swap(products);
                            --contracts[j].a;
                            debit[0] += contracts[j].priceA;
                        } else if (products[i].type == Product::TypeB && contracts[j].b > 0) {
                            products.erase(products.begin() + i);
                            std::vector<Product>(products).swap(products);
                            --contracts[j].b;
                            debit[0] += contracts[j].priceB;
                        } else {
                            ++i;
                        }
                    }
                    contracts.erase(contracts.begin() + j);
                    std::vector<Contract>(contracts).swap(contracts);
                } else {
                    ++j;
                }
            }
            repaint();
//        }
//    } else {
//        QMessageBox msgBox;
//        msgBox.setText("Bad Info");
//        msgBox.setStandardButtons(QMessageBox::Ok);
//        msgBox.exec();
//    }
}

void MainWindow::on_BuyNewProductLine_clicked()
{
    bool ok;
    QString type_product = QInputDialog::getText(this, tr("Type Product"),
                                         tr("Type Product (input A or B):"), QLineEdit::Normal,
                                         "", &ok);
    if (ok && (type_product == "A" || type_product == "B")) {
        QString type_product_line = QInputDialog::getText(this, tr("Type Product Line"),
                                             tr("Type Product Line (input A or B):"), QLineEdit::Normal,
                                             "", &ok);
        if (ok && (type_product_line == "A" || type_product_line == "B")) {
            ProductLine* product_line = new ProductLine(this, type_product, type_product_line, &materials, products);
            if (money >= product_line->price) {
                money -= product_line->price;
                ProductLines.push_back(product_line);
                repaint();
            }
        }
    }
    if (ProductLines.size() > 3)
        ui->BuyNewProductLine->setEnabled(false);
    
}

void MainWindow::on_NewCredit_clicked()
{
    bool ok;
    int credit_time = QInputDialog::getInt(this, tr("Credit time"),
                                         tr("Credit time (input number):"), 0, 0, 1000, 0,
                                         &ok);
    if (ok && credit_time) {
        int credit_money = QInputDialog::getInt(this, tr("Credit money"),
                                             tr("Credit money (input number):"), 0, 0, 1000, 0,
                                             &ok);
        if (ok && credit_money) {
            CreditLines.push_back(CreditLine(credit_time, credit_money));
            money += credit_money;
            repaint();
        }
    }
}

void MainWindow::BuyMaterials_clicked()
{
    add_materials();
    qDebug() << "AAAAA\n";
}

void MainWindow::SaleProducts_clicked()
{
    sale_products();
}

void MainWindow::process(int status)
{
    switch (status) {
    case STATE_DISCONNECTED:
    {
        statusBar()->showMessage(tr("Not connected"));
        dialog.show();
        break;
    }
    case STATE_CONNECTED:
    {
         statusBar()->showMessage(tr("Connected"));
         gui_state = MAIN_STATE;
         ui->NewCredit->show();
         ui->BuyNewProductLine->show();
         ui->Market->show();
         ui->Start->setText("Step");
         repaint();
        break;
    }
    case STATE_INVALID_LOGIN:
    {
        statusBar()->showMessage(tr("Invalid Login"));
        dialog.show();
        break;
    }
    }
}

void MainWindow::update_contract_info(QString contract_info)
{
//     updater->update_contract_info(contract_info);
    QMessageBox msgBox;
    QRegExp rx("(\\d+)[A|a](\\d+)/(\\d+)[B|b](\\d+)/(.*)");
    if (rx.indexIn(contract_info) != -1)
    {
        for (std::size_t i = 0; i < markets.size(); ++i) {
            if (markets[i].name == rx.cap(5).toStdString()) {
                index_current_market = i;
            }
        }
    }
    msgBox.setText(contract_info);
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    int info = msgBox.exec();
    if (info == QMessageBox::Yes) {
        if (rx.indexIn(contract_info) != -1) {
            contracts.push_back({rx.cap(1).toInt(),
                                rx.cap(2).toInt(),
                                rx.cap(3).toInt(),
                                rx.cap(4).toInt() });
        }
    }
    connector->command_send(cmd_type_get_contract, reinterpret_cast<uint8_t*>(&opened_markets[0]), opened_markets.size());
//    connector->

}

void MainWindow::show_change_users(QString list_users)
{
    form.show();
    form.form_ui->label->setText(QString("User List:\n") + list_users);
}

void MainWindow::form_closed()
{
    form.close();
}

ProductLine::ProductLine(MainWindow *_parent, QString _product_type, QString _product_line_type, int *_materials, std::vector<Product> &_products) :
    parent(_parent),
    product_type(_product_type),
    product_line_type(_product_line_type),
    widget(NULL),
    button(NULL),
    MatPerTime(_materials, _products)
{
    price = 0;
    if (product_line_type == "A") {
        price += 10;
        MatPerTime.init(4, product_type);
    } else {
        price += 20;
        MatPerTime.init(2, product_type);
    }

    if (product_type == "A")
        price += 5;
    else
        price += 10;

}

ProductLine::~ProductLine()
{
    if (widget)
        delete widget;
    if (button)
        delete button;
}

void ProductLine::create_empty_triangles_in_product_line(QPainter& p)
{
    int j = -1; int count = 0;
    int finish_count = product_line_type == "A" ? 4 : 2;
    for (int i = 0; i < finish_count; ++i) {
        if (MatPerTime.have_materials[i])
            p.setBrush(QBrush(Qt::red));
        else
            p.setBrush(QBrush(Qt::NoBrush));
        if (finish_count == 2)
            j = 0;
        else if(count > 1) {
            count = 0;
            j *= -1;
        }
        QVector<QPoint> vec;
        int widget_height, widget_y;
        widget_height = widget->height();
        widget_y = widget->y();
        vec.push_back(QPoint(widget->x() + 60 + count * 60, widget->y() + widget->height() * 3 / 4 + 20 + j * 30));
        vec.push_back(QPoint(widget->x() + 60 + count * 60 + 10, widget->y() + widget->height() * 3 / 4 + j* 30));
        vec.push_back(QPoint(widget->x() + 60 + count * 60 + 20, widget->y() + widget->height() * 3 / 4 + 20 + j* 30));
        QPolygon polygon(vec);
        p.drawPolygon(polygon);
        ++count;
    }

}

void ProductLine::DownloadMaterials_clicked()
{
    int count = product_type == "A" ? 1 : 2;
    if (MatPerTime.materials_in_line) {
        if (*MatPerTime.materials_in_line - count >= 0) {
            *MatPerTime.materials_in_line -= count;
            MatPerTime.have_materials[0] = true;
            button->setEnabled(false);
        }
    }
    parent->repaint();
}

ProductLine::_MatPerTime::_MatPerTime(int *_materials, std::vector<Product> &_products)
    : materials_in_line(_materials),
      products(_products)
{
}

void ProductLine::_MatPerTime::init(int count_shop, QString _type)
{
    type = _type == QString("A") ? Product::TypeA : Product::TypeB;
    have_materials.resize(count_shop);
    for (std::deque<bool>::iterator it = have_materials.begin(); it != have_materials.end();
         ++it) {
        *it = false;
    }
}

void ProductLine::_MatPerTime::recount()
{
    if (have_materials.back()) {
        Product t;
        t.type = type;
        products.push_back(t);
    }
    for (std::size_t index = have_materials.size() - 1; index > 0; --index) {
        have_materials[index] = have_materials[index - 1];
        have_materials[index - 1] = false;
    }
}

Dialog::Dialog(QWidget *parent, MainWindow *_parent_window) :
    QDialog(parent),
    parent_window(_parent_window),
    dialog_ui(new Ui::Dialog)
{
    dialog_ui->setupUi(this);
}

void Dialog::on_buttonBox_clicked(QAbstractButton *button)
{
    if (!dialog_ui->LoginEdit->text().isEmpty() && !dialog_ui->PasswordEdit->text().isEmpty()) {
        connector->stop();
        connector->start("81.177.175.71", 5000,
             dialog_ui->LoginEdit->text().toStdString(), dialog_ui->PasswordEdit->text().toStdString());
        parent_window->login = dialog_ui->LoginEdit->text().toStdString();
    } else {
        parent_window->updater->system_state_update(STATE_DISCONNECTED, true);
    }
}

void Dialog::on_pushButton_clicked()
{
     if (!dialog_ui->LoginEdit->text().isEmpty() && !dialog_ui->PasswordEdit->text().isEmpty()) {
         connector->stop();
         connector->start("81.177.175.71", 5000,
              dialog_ui->LoginEdit->text().toStdString(), dialog_ui->PasswordEdit->text().toStdString());
        parent_window->login = dialog_ui->LoginEdit->text().toStdString();
         this->close();
     } else {
         QMessageBox msgBox;
         msgBox.setText("Empty login or password");
         msgBox.setStandardButtons(QMessageBox::Ok);
         msgBox.exec();
     }
}

void GUIUpdater::system_state_update(StateType new_state, bool force)
{
    if (state == new_state)
        return;
    if (!force && state == STATE_INVALID_LOGIN && new_state == STATE_DISCONNECTED)
        return;
    state = new_state;
}

void GUIUpdater::update_contract_info(const std::string &_contract_info)
{
    contract_info = _contract_info;
}

void GUIUpdater::show_usr_list(const std::string &_usr_list)
{
    usr_list = _usr_list;
}

void GUIUpdater::form_closed()
{
    emit requestFormClosed();
}

void GUIUpdater::newLabel() {
    while(1)
    {
        if (state == STATE_DISCONNECTED ||
                state == STATE_CONNECTED ||
                state == STATE_INVALID_LOGIN)
            emit requestNewLabel(state);
        state = STATE_IDLE;
        if (!contract_info.empty()) {
            emit requestNewUpdateInfo(QString(contract_info.c_str()));
            contract_info = "";
        }
        if (!usr_list.empty()) {
             emit requestChangeUsers(QString(usr_list.c_str()));
             usr_list = "";
        }
        Sleep(100);
    }
}

void MainWindow::on_Market_clicked()
{
    if (gui_state == MAIN_STATE) {
        gui_state = MARKET_STATE;
        ui->Market->setText("Go to Production");
        for (std::size_t i = 0; i < markets.size(); ++i) {
            if (markets[i].is_opened())
                opened_markets += markets[i].name + "/";
        }
        connector->command_send(cmd_type_get_contract, reinterpret_cast<uint8_t*>(&opened_markets[0]), opened_markets.size());
    } else {
        gui_state = MAIN_STATE;
        ui->Market->setText("Go to Market");
    }
    repaint();
}

MainWindow::Market::Market(const std::string &_name, QRect _rect, Qt::GlobalColor _color, int *_money) :
    price(1),
    time(1)
{
    init(_name, _rect, _color, _money);
}

MainWindow::Market::Market(const std::string &_name, QRect _rect, Qt::GlobalColor _color, int *_money, int _price, int _time) :
    price(_price),
    time(_time)
{
    init(_name, _rect, _color, _money);
}

void MainWindow::Market::init(const std::string &_name, QRect _rect, Qt::GlobalColor _color, int *_money)
{
    name = _name;
    rect = _rect;
    color = _color;
    money = _money;
    selected = !price;
}

void MainWindow::Market::recount_before()
{
    if (time > 0 && *money > 0 && selected)
        *money -= price;
}

void MainWindow::Market::recount_after()
{
    if (time > 0 && *money > 0 && selected)
        --time;
}

Form::Form(QWidget *parent, MainWindow *_parent_window) :
    QWidget(parent),
    form_ui(new Ui::Form),
    parent_window(_parent_window)
{
    form_ui->setupUi(this);
}

void Form::on_pushButton_clicked()
{
    connector->command_send(cmd_type_formed, reinterpret_cast<uint8_t*>(&parent_window->login[0]), parent_window->login.size());
}
