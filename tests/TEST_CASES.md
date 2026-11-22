# Hướng dẫn kiểm thử ứng dụng đặt vé xem phim IT4062

Rubric có 11 mục, tổng 20 điểm cho nhóm hai người. Theo cách tính của đề, nhóm đạt 20/20 thì tương ứng 10/10 mỗi người. Phần demo bên dưới đi qua cả 11 mục. Các tình huống biên có mã ca riêng để chạy lại và đối chiếu kết quả.

Kiến trúc và hành vi tham chiếu gồm CLI qua TCP, một luồng xử lý cho mỗi client, SQLite, JSON Lines UTF-8, ba vai trò và giao dịch đặt ghế. Luồng Buyer kết thúc bằng mã đặt chỗ, theo phạm vi ghi trong thiết kế. [Slide thiết kế và speaker notes](../IT4062_Design_Review_HUST_style.pptx)

## Đối chiếu bài giảng trong thư mục `slide`

| Bài giảng | Phần hiện thực trong đồ án | Ca kiểm chứng |
| --- | --- | --- |
| [Chapter 3, trang 7–10](<../slide/Chapter 3 IT4062.pptx>) và [Chapter 4, trang 4–18](<../slide/Chapter 4 IT4062.pptx>) | Client gọi `socket` và `connect`. Server gọi `socket`, `bind`, `listen` và `accept`. Hai phía dùng `send`, `recv` và `close` để trao đổi qua TCP. | 01, 02, 38, 43, 47 |
| [Chapter 6, trang 3–12](<../slide/Chapter 6 IT4062.pptx>) và [Lecture Notes, mục 3.4](<../slide/IT4062 Lecture Notes 2025-2026.pdf#page=70>) | Server tạo một `pthread` cho mỗi kết nối. Mutex bảo vệ thao tác ghi log. Giao dịch SQLite bảo vệ dữ liệu ghế khi hai luồng cùng đặt vé. | 06, 28, 33, 35 |
| [Chapter 7, trang 5–7](<../slide/Chapter 7 IT4062.pptx>) | Mỗi luồng đọc socket theo kiểu blocking. Khi một client chờ nhập lệnh, các kết nối khác vẫn có luồng xử lý riêng. | 01, 02, 33 |
| [Lecture Notes, mục 4.3](<../slide/IT4062 Lecture Notes 2025-2026.pdf#page=112>) | Giao thức ứng dụng dạng text, có phiên theo kết nối và dùng mô hình request–response. Đồ án chọn JSON Lines với LF làm ranh giới thông điệp. Bộ đệm xử lý cả trường hợp một request đến qua nhiều lần `recv` và nhiều request đến cùng lúc. | 02–06, 08, 10 |

File `main.c` mở socket lắng nghe và tạo một luồng cho mỗi kết nối. Mỗi luồng giữ phiên của client và đọc từng dòng JSON. File `request_handler.c` kiểm tra thông điệp và quyền, gọi module nghiệp vụ rồi gửi phản hồi. Ca 02 minh họa client thứ hai vẫn nhận phản hồi trong lúc luồng của client đầu đang chờ phần còn lại của thông điệp. [Server main](../src/server/main.c) · [Request handler](../src/server/request_handler.c)

## Kiểm thử tự động

Chạy từ thư mục dự án với Python 3, trình biên dịch C và các thư viện trong [README](../README.md).

```bash
make test
```

Bộ test chạy **47 ca** qua TCP IPv4 trên localhost. Mỗi ca có cơ sở dữ liệu, log và cổng thử riêng. Các ca CLI chạy binary `client` thật. Các ca đồng thời đồng bộ thời điểm gửi yêu cầu và kiểm tra cả phản hồi lẫn SQLite. Kết quả mong đợi là `Ran 47 tests` và `OK`.

Có thể chạy riêng một ca bằng tên in ra khi chạy bộ test.

```bash
python3 tests/ticket_booking_test.py BookingTests.test_33_concurrent_overlapping_and_disjoint_bookings
```

## Đối chiếu rubric

| Mục chấm | Điểm | Ca tự động |
| --- | ---: | --- |
| Kết nối socket client/server | 2 | 01–05, 38, 40, 43, 47 |
| Log tin nhận và tin gửi | 1 | 01, 06 |
| Tạo tài khoản Buyer | 1 | 07, 12, 13, 38, 41 |
| Đăng nhập | 1 | 08, 37–39, 41 |
| Tìm phim theo tên | 2 | 17, 20, 37, 38 |
| Lọc phim theo loại, địa điểm và giờ chiếu | 2 | 18–21, 38, 39 |
| Xem sơ đồ và đặt một hoặc nhiều ghế | 3 | 29–38, 42, 44 |
| Thêm phim với tên, mô tả, thời lượng | 2 | 15, 16, 38 |
| Công bố suất theo phòng và khung giờ | 2 | 21–25, 28, 38 |
| Sửa suất chiếu | 2 | 26, 27, 34, 35, 38, 42 |
| Admin tạo tài khoản và quản lý quyền | 2 | 09–14, 38, 45, 46 |
| **Tổng** | **20** | |

## Demo bằng terminal

### 1. Build và tạo dữ liệu thử riêng

Trong terminal server, chạy từ thư mục dự án. Cổng `43636` dùng riêng cho demo. Lưu lại đường dẫn dữ liệu được in ra để xem log và khởi động lại đúng cơ sở dữ liệu.

```bash
make
project_root="$PWD"
demo_dir="$(mktemp -d /tmp/it4062-ticket.XXXXXX)"
printf 'Demo data: %s\n' "$demo_dir"
(
  cd "$demo_dir" &&
  "$project_root/bin/server" --init-admin admin &&
  "$project_root/bin/server" 43636
)
```

Khi được hỏi mật khẩu Admin, nhập `Admin1234`. Server in dòng đang lắng nghe cổng `43636`. Mở terminal client từ thư mục dự án cho mỗi vai trò. Các bước cần đồng thời hai Buyer có thể dùng hai cửa sổ client trên cùng máy.

```bash
./bin/client 127.0.0.1 43636
```

Client báo đã kết nối và hiện dấu nhắc `tickets>`. Nhập từng dòng lệnh bên dưới rồi trả lời dấu nhắc mật khẩu khi xuất hiện. Các ID ví dụ giả định dùng thư mục demo mới. Khi server trả ID khác, dùng ID thực tế cho các bước sau.

### 2. Admin tạo tài khoản và gán quyền

```text
login admin
create-user manager BUYER
set-role 2 MANAGER
create-user buyer_b BUYER
```

Dùng `Manager1234` cho `manager` và `BuyerB1234` cho `buyer_b`. Kết quả lần lượt là Admin đăng nhập, tạo user #2, đổi user #2 sang MANAGER và tạo user #3. Có thể thử `set-role 999 BUYER` để nhận `NOT_FOUND` và `set-role 1 BUYER` để nhận `PERMISSION_DENIED` vì đây là Admin cuối.

### 3. Manager thêm phim, công bố và sửa suất

Trong terminal Manager, nhập mật khẩu `Manager1234` khi đăng nhập.

```text
login manager
theaters
add-movie "Đại Dương" "Phiêu lưu" 120 "Chuyến đi biển."
add-movie "Đêm Thành Phố" "Tâm lý" 90 "Một đêm trong thành phố."
add-showing 1 1 "2030-01-15 18:00" 3 "2030-01-15 18:00" 1 "2030-01-15 20:00"
add-showing 2 2 "2030-01-15 19:30"
update-showing 2 theater_id=4 starts_at="2030-01-15 19:30"
showings 1
```

`theaters` hiện bốn phòng ở Hà Nội và Đà Nẵng. Hai lệnh thêm phim tạo phim #1 và #2. Các lệnh công bố tạo suất #1–#4. Suất #2 chuyển sang phòng #4, Đà Nẵng lúc 19:30. Hai suất #1 và #3 ở cùng phòng nối đúng biên 120 phút.

```text
add-showing 1 1 "2030-01-15 19:59"
update-showing 3 starts_at="2030-01-15 19:30"
add-showing 1 3 "2030-01-16 18:00" 999 "2030-01-16 18:00"
showings 1
```

Hai lệnh đầu trả `SHOWING_CONFLICT`. Lệnh thứ ba trả `NOT_FOUND`. Kết quả `showings 1` vẫn chỉ có suất #1, #2 và #3, chứng minh cả lô đã rollback khi một phòng không tồn tại.

### 4. Buyer đăng ký, tìm phim và chọn nhiều ghế

Trong terminal Buyer A, dùng `BuyerA1234` khi đăng ký và đăng nhập.

```text
browse
register buyer_a
login buyer_a
search "đẠI dƯƠNG"
search "khong co phim nay"
browse location="Hanoi"
browse time=19:30
browse genre="Phiêu lưu" location="Da Nang" date=2030-01-15 time=19:30
showings 1
seats 2
book 2 A1 A2
seats 2
book 2 A2 A1
book 2 A1 A1
book 2 E1
```

Trước khi đăng nhập, `browse` trả `NOT_LOGGED_IN`. Tìm theo tên tiếng Việt trả phim #1 và từ khóa lạ trả danh sách rỗng. Lọc Hà Nội trả #1, #3, #4. Lọc giờ 19:30 trả #2, #4. Kết hợp bốn bộ lọc chỉ trả suất #2.

Lần mở đầu thấy sơ đồ 4×8 và các nhãn ghế trống. Đặt A1, A2 trả mã đặt chỗ. Mở lại thấy hai ghế thành `XX`. Gửi lại hai ghế đã đặt trả `SEAT_UNAVAILABLE`. Ghế lặp A1 và ghế ngoài sơ đồ E1 trả `INVALID_REQUEST`.

### 5. Hai Buyer kiểm tra tranh ghế và rollback

Trong terminal Buyer B, đăng nhập bằng `BuyerB1234`.

```text
login buyer_b
book 2 B1 A2
seats 2
book 2 B1
```

Lượt B1 + A2 trả `SEAT_UNAVAILABLE` vì A2 đã được Buyer A đặt. Hàng B vẫn hiện B1 trống. Lượt sau đặt riêng B1 thành công. Trong terminal Manager, `update-showing 2 starts_at="2030-01-15 20:00"` trả `SHOWING_HAS_BOOKINGS`.

Trên suất #1 còn trống, thử gửi gần như đồng thời `book 1 B1 B2` ở Buyer A và `book 1 B3 B2` ở Buyer B. Một client nhận mã đặt chỗ. Client kia nhận `SEAT_UNAVAILABLE`. `seats 1` chỉ báo hai ghế của client thắng là `XX`. Ca 33 đồng bộ hai client tự động và kiểm tra cả cơ sở dữ liệu.

### 6. Manager đổi lịch trong lúc Buyer đang xem sơ đồ

1. Buyer A chạy `seats 3` và thấy suất 20:00.
2. Manager chạy `update-showing 3 starts_at="2030-01-15 20:30"` và thấy cập nhật thành công.
3. Buyer A chạy `book 3 C1` và nhận `SHOWING_CHANGED`.
4. Buyer A chạy `seats 3`, xác nhận giờ mới 20:30 rồi chạy lại `book 3 C1`. Lần này nhận mã đặt chỗ.

### 7. Đọc log và xác nhận lưu dữ liệu

Trong terminal quan sát, gán `demo_dir` bằng đường dẫn đã in ở bước 1. Kiểm tra gửi nhận của cùng request ID trên cùng socket.

```bash
grep -E 'CONNECT|RECV|SEND' "$demo_dir/data/server.log" | tail -n 24
grep 'action=LOGIN' "$demo_dir/data/server.log"
```

Log có `RECV`, `SEND`, ID, action, trạng thái phản hồi và mã lỗi khi có. Mật khẩu trong dòng đăng nhập hiện `<redacted>`. Nếu có lệnh `sqlite3`, có thể đối chiếu mô tả phim và các ghế của suất #2.

```bash
sqlite3 "$demo_dir/data/tickets.sqlite3" \
  'SELECT title,description,duration FROM movies ORDER BY id;'
sqlite3 "$demo_dir/data/tickets.sqlite3" \
  'SELECT showing_id,seat,booking_id FROM booked_seats WHERE showing_id=2 ORDER BY seat;'
```

Kết quả suất #2 có A1, A2 cùng mã đặt chỗ và B1 có mã khác. Dừng server bằng Ctrl+C, khởi động lại trong cùng thư mục rồi kết nối client mới và đăng nhập lại. `seats 2` vẫn hiển thị A1, A2 và B1 đã đặt.

```bash
(cd "$demo_dir" && "$project_root/bin/server" 43636)
```

## Đối chiếu giao thức và xử lý đồng thời với slide

| Nội dung | Hành vi hiện tại | Ca kiểm thử |
| --- | --- | --- |
| Quy tắc một ghế cho mỗi suất | Nhãn ghế lấy từ số hàng và cột của phòng. SQLite giữ khóa duy nhất theo cặp suất và nhãn ghế. Một lượt chọn nhiều ghế chỉ được xác nhận khi cả lượt ghi thành công. | 29–33, 36 |
| Khám phá phòng và lịch | Manager dùng `LIST_THEATERS` và các thao tác đọc danh mục. Server phân quyền dựa trên role hiện tại của tài khoản. | 10, 21, 45 |
| Kích cỡ frame 4096 byte | Danh sách dài có `after_id` và `next_after_id`. Frame lỗi không đọc được ID thì phản hồi `id: null`. | 03–05, 20 |
| Sơ đồ cũ sau khi quản lý đổi lịch | `GET_SEATS` trả phòng và giờ hiện tại. CLI gửi lại trong `expected_showing`. Kiểm tra và cấp ghế cùng giao dịch. | 34, 35, 42 |


Ví dụ đặt hai ghế theo định dạng request của slide. Gửi mỗi object thành một dòng và kết thúc bằng LF.

```json
{"id":17,"action":"BOOK_TICKETS","data":{"showing_id":2,"seats":["A1","A2"]}}
```

## Kết nối, giao thức và log

| ID | Thao tác hoặc đầu vào | Kết quả mong đợi |
| --- | --- | --- |
| 01 | Kết nối, gửi một phần JSON rồi đóng chiều gửi. Mở client thứ hai. | Kết nối đầu kết thúc. Client thứ hai gửi lệnh được. Log có IP, sự kiện kết nối và ngắt kết nối. |
| 02 | Chia một request qua hai lần gửi, cho client khác gửi trong lúc chờ và gộp hai request tiếp theo vào cùng lượt gửi TCP. | Client thứ hai nhận phản hồi ngay cả khi client đầu đang gửi dở. Server đợi LF rồi xử lý đủ ba request của client đầu, đúng thứ tự, đúng ID. |
| 03 | Gửi dòng rỗng, JSON hỏng, object thiếu trường, trường sai kiểu, UTF-8 lỗi, NUL thô và dữ liệu thừa sau JSON. | Từng dòng trả `INVALID_REQUEST`. Kết nối vẫn nhận request kế tiếp. |
| 04 | Gửi ID âm, 0, chuỗi, boolean, số lẻ, ID vượt giới hạn và action lạ với ID 2147483647. | ID sai trả `INVALID_REQUEST` với `id: null`. Action lạ trả `UNKNOWN_ACTION` với ID hợp lệ ban đầu. |
| 05 | Gửi frame đúng 4096 byte tính cả LF, frame 4097 byte, frame dài hơn nhiều, rồi một request hợp lệ. | Frame 4096 byte được xử lý. Frame vượt giới hạn trả `MESSAGE_TOO_LARGE`. Frame sau vẫn được đọc đúng. |
| 06 | Đăng ký, đăng nhập, gửi mật khẩu trong trường lồng nhau, gửi JSON hỏng có mật khẩu và yêu cầu đặt ghế cho ID không có. | Có log `RECV` và `SEND` cho cả thành công và lỗi, gồm dữ liệu đặt ghế và mã lỗi. Log che mật khẩu hợp lệ và bỏ nội dung frame hỏng. |

## Tài khoản và phân quyền

| ID | Thao tác hoặc đầu vào | Kết quả mong đợi |
| --- | --- | --- |
| 07 | Thử username dài 2, 3, 20, 21 ký tự, ký tự ngoài tập cho phép và username trùng. Thử mật khẩu dài 5, 6, 32, 33 ký tự, có khoảng trắng thường, khoảng trắng Unicode, ký tự điều khiển, sai kiểu và tiếng Việt hợp lệ. Gửi thêm role `ADMIN` khi tự đăng ký. | Chỉ dữ liệu đúng giới hạn được tạo. Username trùng trả `USERNAME_TAKEN`. Tài khoản tự đăng ký luôn có role `BUYER`. |
| 08 | Dùng lệnh trước đăng nhập, đăng nhập sai mật khẩu hoặc tài khoản chưa tồn tại, đăng nhập đúng, thử đăng nhập lần hai, logout, rồi đăng nhập lại. Mở thêm socket để kiểm tra độc lập. | Phiên chuyển Guest → Buyer → Guest đúng. Các mã lỗi tương ứng là `NOT_LOGGED_IN`, `INVALID_CREDENTIALS`, `INVALID_REQUEST` hoặc `PERMISSION_DENIED`. Phiên mới bắt đầu là Guest. |
| 09 | Khởi tạo admin với mật khẩu ngắn, chạy lại với thông tin đúng, rồi yêu cầu khởi tạo admin thứ hai. | Lệnh đầu thoát lỗi. Các lần sau kết thúc với đúng một admin. Tài khoản ban đầu đăng nhập được. |
| 10 | Admin đổi một tài khoản đang kết nối từ Buyer → Manager → Admin → Buyer. Gửi lệnh của từng role ngay sau mỗi lần đổi. | Server áp dụng quyền mới trên kết nối đang mở. Lệnh ngoài quyền trả `PERMISSION_DENIED`. |
| 11 | Admin tạo tài khoản trùng tên, gửi role sai, ID sai hoặc không tồn tại và thử hạ quyền admin cuối. | Trả `USERNAME_TAKEN`, `INVALID_REQUEST`, `NOT_FOUND` hoặc `PERMISSION_DENIED`. Admin cuối vẫn tồn tại. |
| 12 | Tạo hai tài khoản cùng mật khẩu và đọc SQLite. | Hai salt 16 byte và hai hash 32 byte khác nhau. Tệp SQLite không chứa chuỗi mật khẩu đó. |
| 13 | Hai client cùng đăng ký một username. | Chỉ một thành công. Yêu cầu kia trả `USERNAME_TAKEN`. SQLite có đúng một tài khoản. |
| 14 | Hai admin đồng thời hạ quyền của nhau. | Một thay đổi thành công, một yêu cầu trả `PERMISSION_DENIED`. Còn đúng một admin. |
| 45 | Thử toàn bộ 14 action trên Guest, Buyer, Manager và Admin. Gửi payload rỗng để tách kiểm tra quyền khỏi kiểm tra dữ liệu. | Guest gọi lệnh cần đăng nhập nhận `NOT_LOGGED_IN`. Role sai nhận `PERMISSION_DENIED`. Role hợp lệ tới được bước xử lý action. |

## Phim, tìm kiếm và bộ lọc

| ID | Thao tác hoặc đầu vào | Kết quả mong đợi |
| --- | --- | --- |
| 15 | Thêm phim ở hai biên. Tên 1 và 100 ký tự, thể loại 1 và 30, mô tả 0 và 500, thời lượng 1 và 500 phút. | Cả hai được thêm. SQLite lưu đúng từng trường, kể cả chuỗi Unicode. |
| 16 | Thêm phim thiếu trường, tên hoặc thể loại rỗng, chuỗi vượt giới hạn, thời lượng sai kiểu hoặc ngoài khoảng, chuỗi chứa NUL hoặc ký tự điều khiển. | Trả `INVALID_REQUEST`. Bảng phim vẫn trống. |
| 17 | Tìm một phần tên tiếng Việt với hoa thường khác nhau, chuỗi chứa `%_`, chuỗi không khớp và từ khóa sai kiểu hoặc độ dài. | Khớp theo chuỗi con, không phân biệt hoa thường. Ký tự `%_` được xử lý như ký tự thường. Có danh sách rỗng hoặc lỗi hợp lệ khi tương ứng. |
| 18 | Duyệt với thể loại, địa điểm, ngày, giờ riêng lẻ, kết hợp cả bốn, không có bộ lọc và tổ hợp không có kết quả. Liệt kê theo ID phim. | Các bộ lọc kết hợp theo AND. Dữ liệu trả về đúng suất. |
| 19 | Dùng thể loại hoặc địa điểm lạ, ngày giờ vô hiệu, bộ lọc sai kiểu và ID phim sai hoặc không tồn tại. | Trả `INVALID_FILTER`, `INVALID_REQUEST` hoặc `NOT_FOUND` đúng trường hợp. |
| 20 | Tạo nhiều bản ghi gần giới hạn kích cỡ. Đọc tiếp bằng `after_id` và thử cursor sai ở các endpoint liệt kê. | Các trang không trùng, không sót, theo thứ tự ID. Trang cuối hết cursor. Cursor sai trả `INVALID_REQUEST`. |
| 21 | Liệt kê phòng ban đầu rồi yêu cầu từ sau ID 2. | Có bốn phòng đúng địa điểm và kích thước. Cursor sau ID 2 trả phòng 3 và 4. |

## Lịch chiếu

| ID | Thao tác hoặc đầu vào | Kết quả mong đợi |
| --- | --- | --- |
| 22 | Công bố 10 suất ở nhiều phòng. Thử 0, 11 suất và dữ liệu slot sai kiểu, thiếu trường hoặc ID phòng sai. | 10 suất được tạo theo thứ tự gửi. Các batch sai trả `INVALID_REQUEST`. |
| 23 | Gửi ngày không hợp lệ, ngày nhuận 29/02/2032, giờ 23:59, giờ sai định dạng, phim hoặc phòng không có. | Ngày nhuận và 23:59 được chấp nhận. Dữ liệu sai trả `INVALID_REQUEST`. Phim hoặc phòng không có trả `NOT_FOUND`. |
| 24 | Với phim 120 phút, đặt từ 23:00. Thử giờ trùng, giờ chồng qua nửa đêm, suất nối đúng biên và cùng giờ ở phòng khác. | Hai suất chồng nhau trong cùng phòng trả `SHOWING_CONFLICT`. Suất nối biên và suất ở phòng khác tạo được. |
| 25 | Batch có slot hợp lệ trước, theo sau là slot trùng hoặc phòng không tồn tại. | Toàn bộ batch rollback. Sau đó có thể gửi lại batch hợp lệ. |
| 26 | Sửa giờ, sửa phòng, sửa cả hai. Thử bỏ trống thay đổi, gửi giá trị sai, chuyển sang phòng không có hoặc lịch trùng. | Chỉ cập nhật hợp lệ làm đổi dữ liệu. Lỗi giữ nguyên phòng và giờ cũ. |
| 27 | Đặt ghế rồi yêu cầu đổi giờ hoặc phòng của suất. | Cả hai yêu cầu trả `SHOWING_HAS_BOOKINGS`. Suất giữ lịch cũ. |
| 28 | Hai manager cùng tạo hai suất chồng giờ tại một phòng. | Một yêu cầu thành công, một yêu cầu trả `SHOWING_CONFLICT`. SQLite chỉ có một suất. |

## Sơ đồ ghế và đặt vé

| ID | Thao tác hoặc đầu vào | Kết quả mong đợi |
| --- | --- | --- |
| 29 | Mở hai sơ đồ trống 6×10 và 4×8, đặt ghế cuối từng phòng rồi mở lại. | Mỗi sơ đồ có đúng kích thước và chỉ ghế đã đặt của suất tương ứng. |
| 30 | Thử mảng ghế rỗng, quá 10 ghế, trùng nhãn, nhãn sai, ngoài sơ đồ, ID suất sai và suất không tồn tại. | Trả `INVALID_REQUEST` hoặc `NOT_FOUND`. Không phát sinh bản ghi đặt vé hoặc ghế. |
| 31 | Đặt 10 ghế trong suất đầu và đặt lại A1 của suất khác trong cùng phòng. | Hai mã đặt chỗ khác nhau, đúng tài khoản, đúng suất. Ghế chỉ bị khóa trong suất đã đặt. |
| 32 | Đặt A2. Client khác yêu cầu A1, A2, A3, rồi thử lại A1 và A3. Gửi lại yêu cầu đã thành công. | Lần đầu trả `SEAT_UNAVAILABLE` và rollback A1, A3. Lần thử lại thành công. Gửi lại yêu cầu thành công không tạo thêm lượt đặt. |
| 33 | Hai client đồng thời yêu cầu A1/A2 và A3/A2. Sau đó mỗi client đặt một ghế khác nhau. | Yêu cầu chồng ghế chỉ có một bên thành công. Hai yêu cầu tách ghế cùng thành công. Số booking và ghế khớp. |
| 34 | Xem sơ đồ, đổi lịch, đặt bằng thông tin suất cũ, rồi mở lại sơ đồ. Thử client gửi snapshot sai định dạng. | Snapshot cũ trả `SHOWING_CHANGED` và không tạo booking. Snapshot sai trả `INVALID_REQUEST`. Snapshot mới đặt được. |
| 35 | Buyer đặt ghế từ sơ đồ hiện tại đúng lúc Manager đổi phòng của cùng suất. | Nếu đặt trước thì đổi phòng trả `SHOWING_HAS_BOOKINGS`. Nếu đổi phòng trước thì đặt trả `SHOWING_CHANGED`. SQLite phản ánh đúng giao dịch thắng. |
| 36 | Cho SQLite phát sinh lỗi khi ghi ghế thứ hai của một booking, sau đó bỏ lỗi và gửi lại. | Trả `INTERNAL_ERROR`, rollback cả ghế thứ nhất và booking. Lần thử lại thành công. |
| 37 | Thêm dữ liệu, đăng ký, đặt ghế rồi khởi động lại server và đăng nhập lại. | Phiên cũ hết hiệu lực, tài khoản, phim, suất, mã và ghế đã đặt vẫn còn. |
| 44 | Đặt đủ 32 ghế của một suất ở phòng 4×8, thử đặt thêm A1 và mở lại bằng CLI. | GET_SEATS trả đủ 32 ghế đã đặt. BOOK_TICKETS trả `SEAT_UNAVAILABLE`. CLI hiển thị toàn `XX` và báo hết ghế. |

## Client dòng lệnh

| ID | Thao tác hoặc đầu vào | Kết quả mong đợi |
| --- | --- | --- |
| 38 | Chạy xuyên suốt Admin → Manager → Buyer. Nhập tên có khoảng trắng, mô tả có dấu ngoặc kép, lọc địa điểm và đặt ghế bằng chữ thường. | Ba luồng thành công trên client thật. Mô tả lưu đúng. Sơ đồ đổi từ nhãn ghế trống sang `XX`. |
| 39 | Nhập lệnh lạ, dấu ngoặc chưa đóng, ID sai, bộ lọc lặp, sai mật khẩu và lệnh sau logout. | CLI hiện thông báo sử dụng hoặc mã lỗi phù hợp và tiếp tục nhận lệnh. |
| 40 | Khởi động client và server với số lượng tham số, IP hoặc cổng sai. | Tiến trình thoát với mã khác 0 và thông báo có thể đọc được. |
| 41 | Dùng terminal thật cho `register` và `login` với mật khẩu nhập riêng. | Prompt hỏi mật khẩu, mật khẩu không xuất hiện trong output, thao tác thành công. |
| 42 | Mở sơ đồ của hai suất, đổi giờ suất đầu từ client Manager, rồi đặt suất đó trên CLI Buyer. | CLI giữ thông tin riêng cho từng sơ đồ. Đặt theo giờ cũ bị từ chối. Mở lại thấy giờ mới và đặt được. |
| 43 | Kết nối client vào cổng localhost chưa có server lắng nghe. | CLI báo kết nối thất bại và thoát với mã khác 0. |
| 46 | Từ thư mục sạch, chạy `server --init-admin first_admin` trong terminal, nhập mật khẩu, mở server, đăng nhập Admin và tạo Manager. | Mật khẩu không hiện trên terminal. Server tự tạo dữ liệu, Admin đăng nhập và tạo được Manager. |
| 47 | Cho server đóng kết nối trước khi gửi xong phản hồi hoặc gửi phản hồi có ID sai, trạng thái lạ, `data` sai kiểu hay lỗi thiếu thông báo. | Client CLI báo lỗi và thoát với mã khác 0. |
