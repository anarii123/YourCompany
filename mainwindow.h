#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QAbstractButton>
#include <QDialog>
#include <deque>

namespace Ui {
class MainWindow;
class Dialog;
class Form;
}

struct Product
{
    enum _type { TypeA, TypeB } type;
};

enum StateType {
    STATE_IDLE = -1,
    STATE_DISCONNECTED = 0,
    STATE_CONNECTED,
    STATE_INVALID_LOGIN
};


class MainWindow;

class ProductLine : public QWidget
{
    Q_OBJECT
    MainWindow* parent;
public:
    int price;

    QString product_type;
    QString product_line_type;
    QWidget* widget;
    QWidget* button;
    struct _MatPerTime
    {
        std::deque<bool> have_materials;
        int *materials_in_line;
        std::vector<Product>& products;
        Product::_type type;
        _MatPerTime(int* _materials, std::vector<Product>& _products);

        void init(int count_shop, QString _type);

        void recount();

    } MatPerTime;


    explicit ProductLine(MainWindow* _parent,
                QString _product_type,
                QString _product_line_type,
                int* _materials,
                std::vector<Product>& _products);

    ~ProductLine();

    void create_empty_triangles_in_product_line(QPainter &p);
public slots:
    void DownloadMaterials_clicked();
};

class Form : public QWidget
{
    Q_OBJECT
public:
    explicit Form(QWidget *parent = 0, MainWindow *_parent_window = 0);
    ~Form() { }

private slots:
    void on_pushButton_clicked();

private:
    MainWindow* parent_window;
public:
    Ui::Form *form_ui;
};

class Dialog : public QDialog
{
    Q_OBJECT
public:
    explicit Dialog(QWidget *parent = 0, MainWindow* _parent_window = 0);
    ~Dialog() { }
private slots:
    void on_buttonBox_clicked(QAbstractButton *button);

    void on_pushButton_clicked();

private:
    MainWindow* parent_window;
    Ui::Dialog *dialog_ui;
};

class GUIUpdater : public QObject {
    Q_OBJECT
    StateType state;
    std::string contract_info;
    std::string usr_list;
public:
    explicit GUIUpdater(QObject *parent = 0) : QObject(parent), state(STATE_IDLE) { }
    void system_state_update(StateType new_state, bool force);
    void update_contract_info(const std::string& _contract_info);
    void show_usr_list(const std::string& _usr_list);
    void form_closed();
public slots:
    void newLabel();

signals:
    void requestNewLabel(int);
    void requestNewUpdateInfo(QString);
    void requestChangeUsers(QString);
    void requestFormClosed();
};


class MainWindow : public QMainWindow
{
    Q_OBJECT
    enum
    {
        IDLE_STATE,
        MARKET_STATE,
        MAIN_STATE,
    } gui_state;

//    bool start_activated;
    int money;
    int materials;
    std::vector<Product> products;
    int credit_max_time;
    QWidget* Office;
    QWidget* WarehouseMaterials;
    QWidget* BuyMaterials;
    QWidget* WarehouseProduct;
    QWidget* SaleProducts;

    int debit[4];
    std::list<ProductLine*> ProductLines;

    struct CreditLine
    {
        int time;
        int money;

        CreditLine(int _time, int _money) :
            time(_time),
            money(_money)
        { }
    };

    struct Market
    {
        std::string name;
        int price;
        int time;
        QRect rect;
        Qt::GlobalColor color;
        int *money;
        bool selected;

        Market(const std::string& _name, QRect _rect, Qt::GlobalColor _color, int *_money);

        Market(const std::string& _name, QRect _rect, Qt::GlobalColor _color, int *_money, int _price, int _time);

        void init(const std::string& _name, QRect _rect, Qt::GlobalColor _color, int *_money);

        bool is_opened() { return !time; }

        void recount_before();

        void recount_after();
    };

    struct Contract
    {
        int a;
        int priceA;
        int b;
        int priceB;
    };

    std::vector<Contract> contracts;

    std::vector<Market> markets;
    std::vector<CreditLine> CreditLines;
    bool connect_status;
    int count_year;
    std::string opened_markets;
    std::size_t index_current_market;

public:
    GUIUpdater *updater;
public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();
private:
    void create_circles(QWidget* group_box, QPainter &p, int finish_count, bool have_credit);
    void create_circles_with_name();
    void set_new_group_box(QWidget*& widget, QWidget*& parent, const std::string& name, QRect rect);
    void set_new_button(QWidget*& widget, QWidget*& parent, QRect rect, ProductLine *product_line);
    QRect new_location_product_line();
    void add_materials();
    void sale_products();
    void create_triangles(QWidget *group_box, QPainter& p, void *finish_count);
    void create_square(QWidget *group_box, QPainter& p, int finish_count);
    void createStausBar();

protected:
    void paintEvent(QPaintEvent *);
    void mousePressEvent(QMouseEvent *event);

private slots:
    void on_Start_clicked();

    void on_BuyNewProductLine_clicked();

    void on_NewCredit_clicked();

    void BuyMaterials_clicked();

    void SaleProducts_clicked();

    void on_Market_clicked();

public slots:
    void process(int status);
    void update_contract_info(QString contract_info);
    void show_change_users(QString list_users);
    void form_closed();

private:
    Ui::MainWindow *ui;
public:
    Dialog dialog;
    Form form;
    std::string login;
    std::string password;
};

#endif // MAINWINDOW_H
