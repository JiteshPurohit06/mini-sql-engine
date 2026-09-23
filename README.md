# Mini SQL Engine

A lightweight, in-memory SQL database execution engine implemented in C++. Features a custom Lexer, Parser, Expression AST, and Volcano Iterator execution model with columnar data storage.

## Features

- **Columnar Storage:** In-memory vectorized vector storage supporting `INT32`, `FLOAT64`, and `STRING` data types.
- **Volcano Execution Engine:** Pipelined iterator operators including `Scan`, `Filter`, `Projection`, and `HashAggregate`.
- **Custom SQL Parser:** Recursive descent lexer and parser capable of parsing `SELECT`, `WHERE`, and `GROUP BY` clauses.
- **Boolean Logic Evaluation:** Expression AST handling combined `AND` / `OR` logical predicates.
- **Hash Aggregation:** Group-by query handling with aggregate functions (`COUNT`, `SUM`, `AVG`).

## Query Examples Supported

```sql
SELECT id, age FROM users WHERE age > 25 AND id < 105;
SELECT dept, COUNT(age), AVG(age) FROM users GROUP BY dept;
SELECT dept, SUM(age) FROM users WHERE age > 20 GROUP BY dept;
