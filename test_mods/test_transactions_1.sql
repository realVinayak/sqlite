.open test_mem_2025_06_05.db
-- PRAGMA journal_mode=WAL;
DROP TABLE IF EXISTS sample_table;
CREATE TABLE sample_table (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);
INSERT INTO sample_table (name, age) VALUES ('Alice', 25);
-- .SQL2C_select (text, 0, msg)
select 'READ_1';
-- .SQL2C_select (text, 0, name), (int, 1, age)
select name, age from sample_table;
begin transaction;
INSERT INTO sample_table (name, age) VALUES ('Bob', 29);
-- .SQL2C_select (text, 0, msg)
select 'READ_2';
-- .SQL2C_select (text, 0, name), (int, 1, age)
select name, age from sample_table;
rollback;
-- .SQL2C_select (text, 0, msg)
select 'READ_3';
-- .SQL2C_select (text, 0, name), (int, 1, age)
select name, age from sample_table;
begin transaction;
update sample_table set age = 90;
-- .SQL2C_select (text, 0, msg)
select 'READ_4';
-- .SQL2C_select (text, 0, name), (int, 1, age)
select name, age from sample_table;
savepoint test_1;
update sample_table set age = 91;
-- .SQL2C_select (text, 0, msg)
select 'READ_5';
-- .SQL2C_select (text, 0, name), (int, 1, age)
select name, age from sample_table;
INSERT INTO sample_table (name, age) VALUES ('Bob', 29);
-- .SQL2C_select (text, 0, msg)
select 'READ_6';
-- .SQL2C_select (text, 0, name), (int, 1, age)
select name, age from sample_table;
rollback to test_1;
-- .SQL2C_select (text, 0, msg)
select 'READ_7';
-- .SQL2C_select (text, 0, name), (int, 1, age)
select name, age from sample_table;
-- .SQL2C_select (text, 0, msg)
select 'COUNT_READ_1';
-- .SQL2C_select (int, 0, count)
select count(*) from sample_table;