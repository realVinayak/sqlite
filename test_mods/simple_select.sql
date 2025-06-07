.open test_mem_2025_06_05.db
PRAGMA journal_mode=WAL;
DROP TABLE IF EXISTS sample_table;
CREATE TABLE sample_table (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);
INSERT INTO sample_table (name, age) VALUES ('Alice', 25);
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table;
-- .SQL2C_select (text, 0, msg)
Select 'Starting a transaction!';
begin transaction;
update sample_table set age = 90;
-- .SQL2C_select (text, 0, msg)
Select 'Read in transaction';
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table;
rollback;
-- .SQL2C_select (text, 0, msg)
Select 'Rollback transaction result:';
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table;