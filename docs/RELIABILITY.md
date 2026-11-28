# Xử lý đồng thời, tình huống biên và kiểm thử

Server có thể xử lý nhiều kết nối TCP cùng lúc. Mỗi worker giữ phiên và kết nối SQLite riêng. Độ đúng của dữ liệu dùng chung phụ thuộc vào thứ tự kiểm tra và ghi trong SQL, phạm vi giao dịch và các ràng buộc trong schema. [Vòng đời worker](../src/server/main.c#L20) · [Schema SQLite](../src/server/ticket_db.c#L11)

## Những tình huống đã có kiểm thử

| Tình huống | Đoạn code quyết định kết quả | Ca kiểm thử |
| --- | --- | --- |
| Hai Buyer chọn cùng ghế của một suất | `ticket_book()` mở giao dịch bằng `BEGIN IMMEDIATE`. Khóa chính `(showing_id, seat)` chỉ cho một lượt sở hữu ghế. `create_booking()` phát hiện câu INSERT không thêm được dòng nào và trả `SEAT_UNAVAILABLE`. `ticket_commit()` rollback phần đã ghi của lượt thua. | [32–33](../tests/ticket_booking_test.py#L703) |
| Hai Buyer chọn ghế khác nhau | SQLite cho từng giao dịch ghi lần lượt. Hai lượt đều thành công nếu các ghế đều trống và hợp lệ. Một nhãn ghế có thể được đặt riêng ở từng suất. | [31](../tests/ticket_booking_test.py#L689) · [33](../tests/ticket_booking_test.py#L717) |
| Hai Manager thêm suất chồng giờ | `ticket_add_showing()` lấy quyền ghi trước khi gọi `check_slot()`. Giao dịch xử lý sau nhìn thấy suất đã commit và trả `SHOWING_CONFLICT`. Nếu một slot trong batch lỗi, cả batch rollback. | [24–25](../tests/ticket_booking_test.py#L595) · [28](../tests/ticket_booking_test.py#L644) |
| Buyer đặt lúc Manager đổi lịch | `ticket_book()` và `ticket_update_showing()` đều dùng `BEGIN IMMEDIATE`. Nếu đặt trước, `update_showing()` thấy booking và trả `SHOWING_HAS_BOOKINGS`. Nếu đổi trước, `create_booking()` đối chiếu `expected_showing` và trả `SHOWING_CHANGED` khi thông tin đã cũ. | [34–35](../tests/ticket_booking_test.py#L737) · [42](../tests/ticket_booking_test.py#L913) |
| Hai client đăng ký cùng username | `UNIQUE(username)` và `INSERT OR IGNORE` trong `insert_user()` chỉ tạo một tài khoản. Yêu cầu kia nhận `USERNAME_TAKEN`. | [13](../tests/ticket_booking_test.py#L392) |
| Hai Admin đồng thời hạ quyền nhau | `ticket_set_role()` kiểm tra điều kiện còn Admin khác ngay trong câu UPDATE. Sau một lần hạ quyền thành công, điều kiện chặn hạ quyền Admin cuối. | [14](../tests/ticket_booking_test.py#L401) |
| SQLite lỗi sau khi ghi một phần lượt đặt | `create_booking()` chuyển lỗi SQLite thành `INTERNAL_ERROR`. `ticket_commit()` rollback booking và các ghế đã ghi. | [36](../tests/ticket_booking_test.py#L773) |
| Một client gửi dở trong lúc client khác gửi đủ | Mỗi socket có một worker. `ticket_read_line()` chờ LF của socket đang đọc trong khi worker khác vẫn xử lý được yêu cầu của mình. Nó cũng giữ byte dư để xử lý các dòng tiếp theo đã nhận. | [02](../tests/ticket_booking_test.py#L219) |

### Vì sao ghế của lượt thua vẫn trống

Đoạn cuối của `ticket_book()` lấy quyền ghi trước khi `create_booking()` đọc thông tin suất và ghi ghế. Kết quả của `create_booking()` sau đó quyết định commit hay rollback. [Giao dịch đặt vé](../src/server/ticket_booking.c#L169)

```c
if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
    return ticket_db_error(db);
return ticket_commit(db, create_booking(session, showing_id, seats, count, expected));
```

Giả sử `A2` đã có người đặt. Buyer thứ hai yêu cầu `A1 A2 A3`. Trong `create_booking()`, server tạo dòng `bookings` và ghi được `A1`. Đến `A2`, khóa chính khiến INSERT bỏ qua dòng xung đột và `sqlite3_changes()` trả 0. Hàm dừng ngay ở `A2` nên chưa thử ghi `A3`. `ticket_commit()` rollback dòng `bookings` vừa tạo cùng `A1`. Sau đó sơ đồ chỉ có `A2` bị chiếm, đúng như ca 32 kiểm tra. [Thứ tự ghi trong code](../src/server/ticket_booking.c#L101) · [Rollback](../src/server/ticket_db.c#L140) · [Ca 32](../tests/ticket_booking_test.py#L703)

## Các trường hợp còn mở

Các hành vi bên dưới được xác định từ phạm vi kiểm tra trong mã nguồn. Các ca được dẫn kèm mô tả tình huống gần nhất mà bộ test hiện tại kiểm tra.

| Trường hợp | Hành vi hiện tại và hệ quả |
| --- | --- |
| Quyền bị thu hồi khi request đang chạy | `ticket_handle_request()` gọi `authorize()` rồi mới gọi hàm nghiệp vụ. Nếu quyền đổi sau thời điểm kiểm tra, request đã được duyệt có thể ghi xong. Ca 10 kiểm tra quyền mới với các request gửi sau khi đổi role hoàn tất. [Handler](../src/server/request_handler.c#L150) · [Ca 10](../tests/ticket_booking_test.py#L351) |
| Hai Manager sửa cùng trường của một suất | `update_showing()` đọc lịch hiện tại trong giao dịch và giữ giá trị hiện tại cho trường bị bỏ qua. Hai yêu cầu thay cùng một trường đều có thể thành công nếu từng lịch hợp lệ. Giá trị ghi sau cùng được lưu vì `UPDATE_SHOWING` chưa có phiên bản lịch để phát hiện chỉnh sửa dựa trên dữ liệu cũ. [Đọc và cập nhật suất](../src/server/ticket_catalog.c#L339) |
| Server đã commit nhưng client mất phản hồi | `ticket_commit()` kết thúc trước khi `send_result()` gửi kết quả. Nếu mất kết nối ở khoảng này, người dùng có thể thấy lỗi kết nối dù ghế đã được đặt. `id` chỉ ghép request với response. Gửi lại cùng lựa chọn có thể nhận `SEAT_UNAVAILABLE` mà không lấy lại được mã đặt chỗ cũ qua CLI. Ca 32 kiểm tra gửi lại lựa chọn đã đặt. Ca 47 kiểm tra phản ứng của CLI với phản hồi thiếu hoặc sai. [Kết thúc giao dịch](../src/server/ticket_db.c#L140) · [Gửi kết quả](../src/server/request_handler.c#L150) · [Ca 32](../tests/ticket_booking_test.py#L703) · [Ca 47](../tests/ticket_booking_test.py#L949) |
| Đặt ghế khi chưa mở sơ đồ, hoặc lịch đổi rồi quay lại như cũ | `expected_showing` là tùy chọn. CLI chỉ gửi khi đã nhận sơ đồ cho suất đó. Server đối chiếu phòng và giờ hiện tại với cặp giá trị được gửi. Lịch từng đổi rồi quay lại đúng cặp đó sẽ vẫn khớp. Khóa ghế và kiểm tra kích thước phòng vẫn được áp dụng cho mỗi lượt. [Dữ liệu CLI gửi](../src/client/ticket_cli.c#L363) · [Đối chiếu phòng và giờ](../src/server/ticket_booking.c#L71) |
| SQLite bị giữ khóa ghi lâu | Mỗi kết nối cài `busy_timeout` 5.000 ms. Khi SQLite hết thời gian chờ và trả lỗi cho thao tác ghi, handler có thể trả `INTERNAL_ERROR`. Ca 36 kiểm tra lỗi ghi bên trong giao dịch. [Cấu hình SQLite](../src/server/ticket_db.c#L82) · [Bắt đầu giao dịch đặt vé](../src/server/ticket_booking.c#L169) |
| Client gửi dở hoặc giữ kết nối lâu | `ticket_read_line()` dùng `recv()` blocking. Server chưa đặt hạn chờ đọc cho phiên rảnh hay dòng đang gửi dở. Mỗi socket như vậy vẫn giữ một worker. Khi đạt 100 kết nối đang phục vụ, server đóng kết nối mới nhận. Ca 02 kiểm tra một kết nối gửi dở trong lúc còn chỗ cho client thứ hai. [Đọc TCP](../src/common/ticket_protocol.c#L10) · [Giới hạn kết nối](../src/server/main.c#L75) |
| Suất thay đổi giữa hai trang kết quả | Mỗi trang chạy một truy vấn mới với điều kiện `id > after_id`. Khi có suất mới hoặc suất đổi phòng hay giờ giữa hai lượt, danh sách có thể khác một lần đọc toàn bộ tại một thời điểm. Một suất có ID đã đi qua nhưng vừa trở nên khớp bộ lọc sẽ chỉ xuất hiện nếu duyệt lại từ đầu. Ca 20 kiểm tra phân trang trên tập dữ liệu ổn định. [Truy vấn theo cursor](../src/server/ticket_catalog.c#L121) · [Ca 20](../tests/ticket_booking_test.py#L528) |
| Đặt ghế sau giờ chiếu | Các hàm ngày giờ kiểm tra định dạng và tính hợp lệ của lịch. `create_booking()` đọc giờ để đối chiếu `expected_showing`, chưa so giờ chiếu với thời điểm hiện tại hay một hạn chót bán vé. [Kiểm tra ngày giờ](../src/server/ticket_db.c#L179) · [Kiểm tra trước khi đặt](../src/server/ticket_booking.c#L71) |

## Chạy kiểm thử và đối chiếu dữ liệu

Từ thư mục gốc, chạy `make test` để build và chạy 47 ca tích hợp. Mỗi ca khởi động server trên cổng thử qua localhost trong thư mục tạm, với SQLite và log riêng. Ca CLI chạy binary thật, một số ca mở pseudo-terminal để kiểm tra nhập mật khẩu. `race()` dùng barrier để đưa các yêu cầu vào cùng thời điểm gửi. Các ca đồng thời kiểm tra cả phản hồi và bản ghi sau khi các yêu cầu hoàn tất. [Bộ khởi chạy test](../tests/ticket_booking_test.py#L59) · [Hàm đồng bộ](../tests/ticket_booking_test.py#L188) · [Bảng test và rubric](../tests/TEST_CASES.md)

Để chạy riêng ba ca về xếp lịch và đặt ghế với binary mặc định, dùng các lệnh sau.

```bash
make
python3 tests/ticket_booking_test.py \
  ScheduleTests.test_28_concurrent_scheduling_detects_overlap \
  BookingTests.test_33_concurrent_overlapping_and_disjoint_bookings \
  BookingTests.test_35_concurrent_update_and_booking_use_same_showing_state
```

Nếu chạy demo thủ công, dùng [thư mục dữ liệu riêng](../tests/TEST_CASES.md#1-build-và-tạo-dữ-liệu-thử-riêng). Sau đó mở log và ghép `RECV` với `SEND` bằng `fd` và `id` trong cùng khoảng `CONNECT` đến `DISCONNECT`. Server có thể tái sử dụng số `fd` sau khi đóng kết nối. Dòng `SEND` cho biết lời gọi gửi thành công phía server. Việc client đọc và hiển thị phản hồi diễn ra sau đó trên kết nối. [Ghi log yêu cầu và phản hồi](../src/server/request_handler.c#L90) · [Ghi log kết nối](../src/server/main.c#L75)

```bash
tail -f "$demo_dir/data/server.log"
sqlite3 -readonly "$demo_dir/data/tickets.sqlite3" \
  'SELECT b.id, b.user_id, s.showing_id, s.seat
   FROM bookings AS b JOIN booked_seats AS s ON s.booking_id = b.id
   WHERE s.showing_id = 2 ORDER BY b.id, s.seat;'
```

`tail -f` chạy liên tục. Dùng terminal khác cho truy vấn, hoặc dừng `tail` bằng Ctrl+C trước khi chạy SQLite. Thay `2` bằng ID suất của demo. Dữ liệu và log nằm dưới thư mục làm việc của **server**. Khi khởi động lại ở thư mục khác, server mở cơ sở dữ liệu theo đường dẫn tương đối của thư mục mới. [Đường dẫn dữ liệu](../include/common.h) · [Mở SQLite](../src/server/ticket_db.c#L82)

## Xác định lỗi khi chạy

| Dấu hiệu | Kiểm tra |
| --- | --- |
| Client báo lỗi kết nối | Kiểm tra server đã in thông báo lắng nghe và hai đầu dùng cùng cổng. CLI nhận địa chỉ IPv4, ví dụ `127.0.0.1`. |
| Server báo cổng đang được sử dụng | Chọn cổng khác cho cả hai chương trình, ví dụ `./bin/server 43636` và `./bin/client 127.0.0.1 43636`. |
| Đăng nhập lại thất bại sau khi chuyển thư mục | Đối chiếu thư mục làm việc chứa `data/tickets.sqlite3` và xác nhận đã khởi tạo đúng Admin với `--init-admin`. |
| Đặt ghế báo `SHOWING_CHANGED` hoặc `SEAT_UNAVAILABLE` | Mở lại `seats SHOWING_ID` để xem lịch và ghế hiện tại rồi chọn lại. |
| Mất phản hồi ngay sau khi gửi lệnh đặt | Mở kết nối mới và xem lại sơ đồ. Trong môi trường demo, đối chiếu log và truy vấn booking theo suất, ghế và người dùng để xác định giao dịch đã commit hay chưa. |
| Test dừng với `PermissionError` tại `socket.bind()` | Kiểm tra môi trường có cho tiến trình test mở cổng TCP localhost. |

Server lắng nghe trên mọi giao diện IPv4 và giao thức truyền JSON trực tiếp qua TCP. Dùng mật khẩu demo và môi trường cục bộ hoặc mạng lớp học được kiểm soát. [Thiết lập socket server](../src/server/main.c#L50) · [Định dạng trên dây](PROTOCOL.md#định-dạng-thông-điệp)
