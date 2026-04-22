# sqlpd 사용법

미니 DBMS HTTP API 서버. 기존 SQL 엔진 앞에 HTTP 레이어를 얹어, curl로 SQL을 보내면 SELECT는 터미널에서 읽기 좋은 표로, INSERT/에러는 JSON으로 결과를 돌려준다.

---

## 1. 빌드

```
make
```

`sqlp`(CLI, 기존)와 `sqlpd`(서버) 두 바이너리가 생성된다. 소스가 이미 빌드돼 있으면 `Nothing to be done for 'all'.` 이 나오는데 이게 정상이다. 강제로 새로 빌드하려면:

```
make clean && make
```

---

## 2. 데이터 파일 준비

엔진은 테이블 데이터를 `data/<table>.dat` 에서 읽는다. 처음 실행하기 전에 빈 파일을 만들어 둔다:

```
mkdir -p data
: > data/users.dat
```

부하 테스트용 대용량 데이터는 명령어로 만들고 지울 수 있다:

```
make seed_users                 # 기본 200,000 rows 생성
make seed_users ROWS=100000     # 100,000 rows 생성
make clear_users                # data/users.dat 비우기
```

스키마는 `schema/users.schema`에 정의돼 있다:


| 컬럼    | 타입           |
| ----- | ------------ |
| id    | INT          |
| name  | VARCHAR(64)  |
| age   | INT          |
| email | VARCHAR(128) |


---

## 3. 서버 기동

```
./sqlpd <port> [workers] [queue_capacity]
```

- `port` — HTTP 리스닝 포트 (필수)
- `workers` — 워커 스레드 개수 (선택, 기본 4)
- `queue_capacity` — 대기 큐 크기 (선택, 기본 128)

예:

```
./sqlpd 8080
./sqlpd 8080 8 256
```

`Ctrl+C` 로 graceful shutdown (진행 중 요청은 마저 처리한 뒤 종료).

---

## 4. curl 테스트

옵션 순서는 상관없고, 줄바꿈이 꼬이기 쉬우니 **한 줄로** 쓰는 게 편하다.

### INSERT

```
curl http://localhost:8080/sql -d "INSERT INTO users VALUES (1, 'alice', 30, 'alice@example.com');"
```

성공 응답:

```
{"ok":true,"type":"insert","affected_rows":1}
```

### SELECT (전체)

```
curl http://localhost:8080/sql -d 'SELECT * FROM users;'
```

성공 응답:

```
+----+-------+-----+-------------------+
| id | name  | age | email             |
+----+-------+-----+-------------------+
| 1  | alice | 30  | alice@example.com |
+----+-------+-----+-------------------+
(1 rows)
```

### SELECT (WHERE)

```
curl http://localhost:8080/sql -d 'SELECT * FROM users WHERE id = 1;'
curl http://localhost:8080/sql -d 'SELECT * FROM users WHERE age BETWEEN 20 AND 40;'
curl http://localhost:8080/sql -d 'SELECT id, name FROM users WHERE id = 1;'
```

### GET으로도 가능

```
curl --get --data-urlencode 'sql=SELECT * FROM users;' http://localhost:8080/sql
```

### 응답 헤더까지 보고 싶을 때

`-i` 추가:

```
curl -i http://localhost:8080/sql -d 'SELECT * FROM users;'
```

---

## 5. 에러 응답 예시

모든 에러는 동일한 포맷:

```
{"ok":false,"error":{"code":"...","message":"..."}}
```


| 상황                     | HTTP | code                  |
| ---------------------- | ---- | --------------------- |
| 잘못된 SQL                | 400  | `BAD_SQL`             |
| body/SQL 누락            | 400  | `BAD_REQUEST`         |
| 없는 테이블                 | 404  | `SCHEMA_NOT_FOUND`    |
| 없는 경로 (`/sql` 외)       | 404  | `NOT_FOUND`           |
| 지원 안 하는 SQL (UPDATE 등) | 501  | `SQL_NOT_IMPLEMENTED` |
| 큐 포화                   | 503  | `SERVICE_UNAVAILABLE` |


확인해 보기:

```
curl -i http://localhost:8080/sql -d 'SELEC * FROM users;'         # 400 BAD_SQL
curl -i http://localhost:8080/sql -d 'SELECT * FROM ghosts;'       # 404 SCHEMA_NOT_FOUND
curl -i http://localhost:8080/sql -d 'UPDATE users SET age=99;'    # 501 SQL_NOT_IMPLEMENTED
```

---

## 6. 한 세션 전체 예시

```
# 터미널 A
make
mkdir -p data && : > data/users.dat
./sqlpd 8080

# 터미널 B
curl http://localhost:8080/sql -d "INSERT INTO users VALUES (1, 'alice', 30, 'alice@example.com');"
curl http://localhost:8080/sql -d "INSERT INTO users VALUES (2, 'bob',   25, 'bob@example.com');"
curl http://localhost:8080/sql -d "INSERT INTO users VALUES (3, 'carol', 40, 'carol@example.com');"
curl http://localhost:8080/sql -d 'SELECT * FROM users;'
curl http://localhost:8080/sql -d 'SELECT * FROM users WHERE id = 2;'
curl http://localhost:8080/sql -d 'SELECT * FROM users WHERE age BETWEEN 20 AND 35;'

# 터미널 A에서 Ctrl+C
```

# 20만 rows 생성

```
make seed_users
```

# 원하는 row 수로 생성

```
make seed_users ROWS=20000
```

# users 데이터 비우기

```
make clear_users
```

# ab write 비교용 명령어

# 1. 데이터 초기화
```
make clear_users
```
# 2. INSERT body 파일 생성
```
printf "INSERT INTO users VALUES (1, 'bench_user', 31, 'bench_user@example.com');" > /tmp/insert.sql
```
# 3. 싱글 worker 서버 실행
```
./sqlpd 8080 1 128
```

# 4. ab 쓰기 테스트
```
ab -n 100000 -c 100 \
  -p /tmp/insert.sql \
  -T "text/plain" \
  http://127.0.0.1:8080/sql
```
# 5. 멀티 worker 비교용
# 위 서버를 Ctrl+C로 끈 뒤 실행
```
make clear_users
./sqlpd 8080 8 128
```
# 6. 같은 ab 테스트 반복
```
ab -n 100000 -c 100 \
  -p /tmp/insert.sql \
  -T "text/plain" \
  http://127.0.0.1:8080/sql
```
