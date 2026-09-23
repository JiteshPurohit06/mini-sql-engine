#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <unordered_map>
#include <algorithm>
#include <stdexcept>
#include <iomanip>

// ============================================================================
// 1. DATA TYPES & COLUMN STORAGE
// ============================================================================
enum class DataType { INT32, FLOAT64, STRING };

struct ScalarValue {
    DataType type;
    int32_t int_val = 0;
    double float_val = 0.0;
    std::string str_val = "";

    bool operator==(const ScalarValue& o) const {
        if (type != o.type) return false;
        if (type == DataType::INT32) return int_val == o.int_val;
        if (type == DataType::FLOAT64) return float_val == o.float_val;
        return str_val == o.str_val;
    }
};

struct ScalarHash {
    size_t operator()(const ScalarValue& v) const {
        if (v.type == DataType::INT32) return std::hash<int32_t>{}(v.int_val);
        if (v.type == DataType::FLOAT64) return std::hash<double>{}(v.float_val);
        return std::hash<std::string>{}(v.str_val);
    }
};

class Column {
public:
    virtual ~Column() = default;
    virtual DataType get_type() const = 0;
    virtual size_t size() const = 0;
    virtual ScalarValue get_value(size_t index) const = 0;
};

template <typename T>
class TypedColumn : public Column {
private:
    std::vector<T> data_;
    DataType type_;

public:
    TypedColumn(DataType type) : type_(type) {}

    void append(T value) { data_.push_back(value); }
    const T& get(size_t index) const { return data_[index]; }
    DataType get_type() const override { return type_; }
    size_t size() const override { return data_.size(); }
    ScalarValue get_value(size_t index) const override;
};

template <>
inline ScalarValue TypedColumn<int32_t>::get_value(size_t index) const {
    ScalarValue val; val.type = type_; val.int_val = data_[index]; return val;
}

template <>
inline ScalarValue TypedColumn<double>::get_value(size_t index) const {
    ScalarValue val; val.type = type_; val.float_val = data_[index]; return val;
}

template <>
inline ScalarValue TypedColumn<std::string>::get_value(size_t index) const {
    ScalarValue val; val.type = type_; val.str_val = data_[index]; return val;
}

// ============================================================================
// 2. TABLE & BATCH STRUCTURES
// ============================================================================
class Table {
private:
    std::string name_;
    std::unordered_map<std::string, std::shared_ptr<Column>> columns_;
    size_t row_count_ = 0;

public:
    Table(const std::string& name) : name_(name) {}

    void add_column(const std::string& col_name, std::shared_ptr<Column> col) {
        if (row_count_ == 0) row_count_ = col->size();
        columns_[col_name] = col;
    }

    std::shared_ptr<Column> get_column(const std::string& col_name) const {
        auto it = columns_.find(col_name);
        return (it != columns_.end()) ? it->second : nullptr;
    }

    const std::unordered_map<std::string, std::shared_ptr<Column>>& get_all_columns() const {
        return columns_;
    }

    size_t row_count() const { return row_count_; }
};

struct RecordBatch {
    std::unordered_map<std::string, std::shared_ptr<Column>> columns;
    size_t start_row = 0;
    size_t size = 0;
};

// ============================================================================
// 3. EXPRESSION AST FOR COMPLEX WHERE CLAUSES (AND/OR)
// ============================================================================
enum class OpType { GT, LT, EQ };

class Expr {
public:
    virtual ~Expr() = default;
    virtual bool evaluate(const RecordBatch& batch, size_t row_idx) const = 0;
};

class BinaryPredicateExpr : public Expr {
private:
    std::string col_name_;
    OpType op_;
    int32_t val_;

public:
    BinaryPredicateExpr(std::string col, OpType op, int32_t val)
        : col_name_(col), op_(op), val_(val) {}

    bool evaluate(const RecordBatch& batch, size_t row_idx) const override {
        auto it = batch.columns.find(col_name_);
        if (it == batch.columns.end()) return false;

        auto col = std::dynamic_pointer_cast<TypedColumn<int32_t>>(it->second);
        if (!col) return false;

        int32_t cell_val = col->get(batch.start_row + row_idx);
        if (op_ == OpType::GT) return cell_val > val_;
        if (op_ == OpType::LT) return cell_val < val_;
        if (op_ == OpType::EQ) return cell_val == val_;
        return false;
    }
};

enum class LogicOp { AND, OR };

class LogicalExpr : public Expr {
private:
    std::shared_ptr<Expr> left_;
    std::shared_ptr<Expr> right_;
    LogicOp op_;

public:
    LogicalExpr(std::shared_ptr<Expr> left, std::shared_ptr<Expr> right, LogicOp op)
        : left_(left), right_(right), op_(op) {}

    bool evaluate(const RecordBatch& batch, size_t row_idx) const override {
        if (op_ == LogicOp::AND) return left_->evaluate(batch, row_idx) && right_->evaluate(batch, row_idx);
        if (op_ == LogicOp::OR) return left_->evaluate(batch, row_idx) || right_->evaluate(batch, row_idx);
        return false;
    }
};

// ============================================================================
// 4. VOLCANO EXECUTION OPERATORS
// ============================================================================
class Operator {
public:
    virtual ~Operator() = default;
    virtual void open() = 0;
    virtual std::shared_ptr<RecordBatch> next() = 0;
    virtual void close() = 0;
};

class ScanOperator : public Operator {
private:
    std::shared_ptr<Table> table_;
    size_t current_row_ = 0;
    size_t batch_size_ = 1024;

public:
    ScanOperator(std::shared_ptr<Table> table, size_t batch_size = 1024)
        : table_(table), batch_size_(batch_size) {}

    void open() override { current_row_ = 0; }

    std::shared_ptr<RecordBatch> next() override {
        if (current_row_ >= table_->row_count()) return nullptr;

        size_t rows_to_read = std::min(batch_size_, table_->row_count() - current_row_);
        auto batch = std::make_shared<RecordBatch>();
        batch->start_row = current_row_;
        batch->size = rows_to_read;

        for (const auto& pair : table_->get_all_columns()) {
            batch->columns[pair.first] = pair.second;
        }

        current_row_ += rows_to_read;
        return batch;
    }

    void close() override {}
};

class FilterOperator : public Operator {
private:
    std::unique_ptr<Operator> child_;
    std::shared_ptr<Expr> predicate_;

public:
    FilterOperator(std::unique_ptr<Operator> child, std::shared_ptr<Expr> pred)
        : child_(std::move(child)), predicate_(pred) {}

    void open() override { child_->open(); }

    std::shared_ptr<RecordBatch> next() override {
        while (auto batch = child_->next()) {
            std::unordered_map<std::string, std::shared_ptr<Column>> filtered_cols;
            
            // Build dynamic target columns
            for (const auto& pair : batch->columns) {
                if (pair.second->get_type() == DataType::INT32) {
                    filtered_cols[pair.first] = std::make_shared<TypedColumn<int32_t>>(DataType::INT32);
                } else if (pair.second->get_type() == DataType::STRING) {
                    filtered_cols[pair.first] = std::make_shared<TypedColumn<std::string>>(DataType::STRING);
                }
            }

            size_t match_count = 0;
            for (size_t i = 0; i < batch->size; ++i) {
                if (predicate_->evaluate(*batch, i)) {
                    match_count++;
                    size_t actual_idx = batch->start_row + i;
                    for (const auto& pair : batch->columns) {
                        if (pair.second->get_type() == DataType::INT32) {
                            auto src = std::dynamic_pointer_cast<TypedColumn<int32_t>>(pair.second);
                            auto dst = std::dynamic_pointer_cast<TypedColumn<int32_t>>(filtered_cols[pair.first]);
                            dst->append(src->get(actual_idx));
                        } else if (pair.second->get_type() == DataType::STRING) {
                            auto src = std::dynamic_pointer_cast<TypedColumn<std::string>>(pair.second);
                            auto dst = std::dynamic_pointer_cast<TypedColumn<std::string>>(filtered_cols[pair.first]);
                            dst->append(src->get(actual_idx));
                        }
                    }
                }
            }

            if (match_count > 0) {
                auto out_batch = std::make_shared<RecordBatch>();
                out_batch->columns = filtered_cols;
                out_batch->start_row = 0;
                out_batch->size = match_count;
                return out_batch;
            }
        }
        return nullptr;
    }

    void close() override { child_->close(); }
};

class ProjectionOperator : public Operator {
private:
    std::unique_ptr<Operator> child_;
    std::vector<std::string> target_columns_;

public:
    ProjectionOperator(std::unique_ptr<Operator> child, std::vector<std::string> target_columns)
        : child_(std::move(child)), target_columns_(target_columns) {}

    void open() override { child_->open(); }

    std::shared_ptr<RecordBatch> next() override {
        auto batch = child_->next();
        if (!batch) return nullptr;

        auto projected_batch = std::make_shared<RecordBatch>();
        projected_batch->start_row = batch->start_row;
        projected_batch->size = batch->size;

        for (const auto& col_name : target_columns_) {
            auto it = batch->columns.find(col_name);
            if (it != batch->columns.end()) {
                projected_batch->columns[col_name] = it->second;
            } else {
                throw std::runtime_error("Projection Error: Column '" + col_name + "' not found.");
            }
        }
        return projected_batch;
    }

    void close() override { child_->close(); }
};

enum class AggType { COUNT, SUM, AVG };

struct AggFunc {
    AggType type;
    std::string column;
    std::string alias;
};

class HashAggregateOperator : public Operator {
private:
    std::unique_ptr<Operator> child_;
    std::string group_by_col_;
    std::vector<AggFunc> agg_funcs_;
    bool executed_ = false;

    struct Accumulator {
        double sum = 0.0;
        size_t count = 0;
    };

public:
    HashAggregateOperator(std::unique_ptr<Operator> child, std::string group_by_col, std::vector<AggFunc> funcs)
        : child_(std::move(child)), group_by_col_(group_by_col), agg_funcs_(funcs) {}

    void open() override { child_->open(); executed_ = false; }

    std::shared_ptr<RecordBatch> next() override {
        if (executed_) return nullptr;

        std::unordered_map<ScalarValue, std::vector<Accumulator>, ScalarHash> hash_table;

        while (auto batch = child_->next()) {
            std::shared_ptr<Column> group_col = nullptr;
            if (!group_by_col_.empty()) {
                group_col = batch->columns[group_by_col_];
            }

            for (size_t i = 0; i < batch->size; ++i) {
                ScalarValue g_key = group_col ? group_col->get_value(batch->start_row + i) : ScalarValue{DataType::STRING, 0, 0.0, "GLOBAL"};

                if (hash_table.find(g_key) == hash_table.end()) {
                    hash_table[g_key] = std::vector<Accumulator>(agg_funcs_.size());
                }

                for (size_t f = 0; f < agg_funcs_.size(); ++f) {
                    auto target_col = batch->columns[agg_funcs_[f].column];
                    double val = 0.0;
                    if (target_col) {
                        val = target_col->get_value(batch->start_row + i).int_val;
                    }
                    hash_table[g_key][f].sum += val;
                    hash_table[g_key][f].count += 1;
                }
            }
        }

        executed_ = true;

        auto out_batch = std::make_shared<RecordBatch>();
        out_batch->start_row = 0;
        out_batch->size = hash_table.size();

        // Prepare grouping output column
        if (!group_by_col_.empty()) {
            auto g_out = std::make_shared<TypedColumn<std::string>>(DataType::STRING);
            for (const auto& pair : hash_table) {
                g_out->append(pair.first.str_val);
            }
            out_batch->columns[group_by_col_] = g_out;
        }

        // Prepare aggregation result columns
        for (size_t f = 0; f < agg_funcs_.size(); ++f) {
            if (agg_funcs_[f].type == AggType::AVG) {
                auto col = std::make_shared<TypedColumn<double>>(DataType::FLOAT64);
                for (const auto& pair : hash_table) {
                    const auto& acc = pair.second[f];
                    col->append(acc.count > 0 ? acc.sum / acc.count : 0.0);
                }
                out_batch->columns[agg_funcs_[f].alias] = col;
            } else {
                auto col = std::make_shared<TypedColumn<int32_t>>(DataType::INT32);
                for (const auto& pair : hash_table) {
                    const auto& acc = pair.second[f];
                    int32_t res = (agg_funcs_[f].type == AggType::COUNT) ? static_cast<int32_t>(acc.count) : static_cast<int32_t>(acc.sum);
                    col->append(res);
                }
                out_batch->columns[agg_funcs_[f].alias] = col;
            }
        }

        return out_batch;
    }

    void close() override { child_->close(); }
};

// ============================================================================
// 5. SQL LEXER
// ============================================================================
enum class TokenType {
    SELECT, FROM, WHERE, GROUP, BY, AND, OR,
    COUNT, SUM, AVG, IDENTIFIER, NUMBER, OPERATOR, COMMA, LPAREN, RPAREN, EOF_TOKEN
};

struct Token {
    TokenType type;
    std::string value;
};

class Lexer {
private:
    std::string src_;
    size_t pos_ = 0;

public:
    Lexer(std::string src) : src_(src) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;

        while (pos_ < src_.length()) {
            char c = src_[pos_];
            if (isspace(c)) { pos_++; continue; }

            if (isalpha(c)) {
                std::string ident;
                while (pos_ < src_.length() && isalnum(src_[pos_])) {
                    ident += src_[pos_++];
                }
                if (ident == "SELECT") tokens.push_back({TokenType::SELECT, ident});
                else if (ident == "FROM") tokens.push_back({TokenType::FROM, ident});
                else if (ident == "WHERE") tokens.push_back({TokenType::WHERE, ident});
                else if (ident == "GROUP") tokens.push_back({TokenType::GROUP, ident});
                else if (ident == "BY") tokens.push_back({TokenType::BY, ident});
                else if (ident == "AND") tokens.push_back({TokenType::AND, ident});
                else if (ident == "OR") tokens.push_back({TokenType::OR, ident});
                else if (ident == "COUNT") tokens.push_back({TokenType::COUNT, ident});
                else if (ident == "SUM") tokens.push_back({TokenType::SUM, ident});
                else if (ident == "AVG") tokens.push_back({TokenType::AVG, ident});
                else tokens.push_back({TokenType::IDENTIFIER, ident});
            } else if (isdigit(c)) {
                std::string num;
                while (pos_ < src_.length() && isdigit(src_[pos_])) {
                    num += src_[pos_++];
                }
                tokens.push_back({TokenType::NUMBER, num});
            } else if (c == '>' || c == '<' || c == '=') {
                tokens.push_back({TokenType::OPERATOR, std::string(1, c)});
                pos_++;
            } else if (c == ',') {
                tokens.push_back({TokenType::COMMA, ","});
                pos_++;
            } else if (c == '(') {
                tokens.push_back({TokenType::LPAREN, "("});
                pos_++;
            } else if (c == ')') {
                tokens.push_back({TokenType::RPAREN, ")"});
                pos_++;
            } else {
                pos_++;
            }
        }
        tokens.push_back({TokenType::EOF_TOKEN, ""});
        return tokens;
    }
};

// ============================================================================
// 6. PARSER WITH PRECEDENCE-BASED EXPRESSION PARSING
// ============================================================================
class Parser {
private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    Token consume(TokenType expected, const std::string& err_msg) {
        if (tokens_[pos_].type == expected) return tokens_[pos_++];
        throw std::runtime_error("Parser Error: " + err_msg);
    }

    std::shared_ptr<Expr> parse_predicate() {
        Token col = consume(TokenType::IDENTIFIER, "Expected column name in WHERE clause");
        Token op_token = consume(TokenType::OPERATOR, "Expected operator (>, <, =)");
        Token val_token = consume(TokenType::NUMBER, "Expected integer value");

        OpType op = OpType::GT;
        if (op_token.value == "<") op = OpType::LT;
        else if (op_token.value == "=") op = OpType::EQ;

        return std::make_shared<BinaryPredicateExpr>(col.value, op, std::stoi(val_token.value));
    }

    std::shared_ptr<Expr> parse_expression() {
        auto left = parse_predicate();

        while (tokens_[pos_].type == TokenType::AND || tokens_[pos_].type == TokenType::OR) {
            LogicOp log_op = (tokens_[pos_].type == TokenType::AND) ? LogicOp::AND : LogicOp::OR;
            pos_++;
            auto right = parse_predicate();
            left = std::make_shared<LogicalExpr>(left, right, log_op);
        }
        return left;
    }

public:
    Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    std::unique_ptr<Operator> parse(const std::unordered_map<std::string, std::shared_ptr<Table>>& catalog) {
        consume(TokenType::SELECT, "Expected 'SELECT'");

        bool has_aggregates = false;
        std::vector<AggFunc> agg_funcs;
        std::vector<std::string> select_columns;

        while (true) {
            if (tokens_[pos_].type == TokenType::COUNT || tokens_[pos_].type == TokenType::SUM || tokens_[pos_].type == TokenType::AVG) {
                has_aggregates = true;
                Token func_token = tokens_[pos_++];
                AggType type = AggType::COUNT;
                if (func_token.type == TokenType::SUM) type = AggType::SUM;
                if (func_token.type == TokenType::AVG) type = AggType::AVG;

                consume(TokenType::LPAREN, "Expected '('");
                Token target = consume(TokenType::IDENTIFIER, "Expected target column inside aggregate");
                consume(TokenType::RPAREN, "Expected ')'");

                std::string alias = func_token.value + "(" + target.value + ")";
                agg_funcs.push_back({type, target.value, alias});
            } else {
                select_columns.push_back(consume(TokenType::IDENTIFIER, "Expected column name").value);
            }

            if (tokens_[pos_].type == TokenType::COMMA) {
                consume(TokenType::COMMA, "");
            } else {
                break;
            }
        }

        consume(TokenType::FROM, "Expected 'FROM'");
        Token table_name = consume(TokenType::IDENTIFIER, "Expected table name");

        auto cat_it = catalog.find(table_name.value);
        if (cat_it == catalog.end()) throw std::runtime_error("Table not found: " + table_name.value);

        // Base Scan Plan
        std::unique_ptr<Operator> plan = std::unique_ptr<Operator>(new ScanOperator(cat_it->second, 2));

        // Optional WHERE clause
        if (tokens_[pos_].type == TokenType::WHERE) {
            consume(TokenType::WHERE, "");
            auto expr = parse_expression();
            plan = std::unique_ptr<Operator>(new FilterOperator(std::move(plan), expr));
        }

        // Optional GROUP BY clause
        std::string group_col = "";
        if (tokens_[pos_].type == TokenType::GROUP) {
            consume(TokenType::GROUP, "");
            consume(TokenType::BY, "Expected 'BY' after 'GROUP'");
            group_col = consume(TokenType::IDENTIFIER, "Expected column name after 'GROUP BY'").value;
        }

        // Assemble Final Root Operator
        if (has_aggregates || !group_col.empty()) {
            plan = std::unique_ptr<Operator>(new HashAggregateOperator(std::move(plan), group_col, agg_funcs));
        } else {
            plan = std::unique_ptr<Operator>(new ProjectionOperator(std::move(plan), select_columns));
        }

        return plan;
    }
};

// ============================================================================
// 7. PIPELINE EXECUTION ENGINE & RUNTIME
// ============================================================================
void execute_query(const std::string& sql, const std::unordered_map<std::string, std::shared_ptr<Table>>& catalog) {
    std::cout << "\n========================================================\n";
    std::cout << "Executing Query: \"" << sql << "\"\n";
    std::cout << "========================================================\n";

    try {
        Lexer lexer(sql);
        auto tokens = lexer.tokenize();

        Parser parser(tokens);
        auto plan = parser.parse(catalog);

        plan->open();
        std::cout << "--- Query Results ---\n";
        bool found = false;
        while (auto batch = plan->next()) {
            for (size_t row = 0; row < batch->size; ++row) {
                found = true;
                std::cout << "Row " << row + 1 << " -> ";
                for (const auto& pair : batch->columns) {
                    std::cout << pair.first << ": ";
                    if (pair.second->get_type() == DataType::INT32) {
                        auto col = std::dynamic_pointer_cast<TypedColumn<int32_t>>(pair.second);
                        std::cout << col->get(row) << "  ";
                    } else if (pair.second->get_type() == DataType::FLOAT64) {
                        auto col = std::dynamic_pointer_cast<TypedColumn<double>>(pair.second);
                        std::cout << std::fixed << std::setprecision(2) << col->get(row) << "  ";
                    } else if (pair.second->get_type() == DataType::STRING) {
                        auto col = std::dynamic_pointer_cast<TypedColumn<std::string>>(pair.second);
                        std::cout << col->get(row) << "  ";
                    }
                }
                std::cout << "\n";
            }
        }
        if (!found) std::cout << "(No matching rows)\n";
        plan->close();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
    }
}

int main() {
    // Populate "users" table
    auto id_column = std::make_shared<TypedColumn<int32_t>>(DataType::INT32);
    id_column->append(101); id_column->append(102); id_column->append(103);
    id_column->append(104); id_column->append(105); id_column->append(106);

    auto age_column = std::make_shared<TypedColumn<int32_t>>(DataType::INT32);
    age_column->append(22); age_column->append(35); age_column->append(28);
    age_column->append(40); age_column->append(18); age_column->append(50);

    auto dept_column = std::make_shared<TypedColumn<std::string>>(DataType::STRING);
    dept_column->append("HR"); dept_column->append("ENG"); dept_column->append("ENG");
    dept_column->append("HR"); dept_column->append("ENG"); dept_column->append("HR");

    auto table = std::make_shared<Table>("users");
    table->add_column("id", id_column);
    table->add_column("age", age_column);
    table->add_column("dept", dept_column);

    std::unordered_map<std::string, std::shared_ptr<Table>> catalog;
    catalog["users"] = table;

    // Test Complete Feature Suite!
    execute_query("SELECT id, age FROM users WHERE age > 25 AND id < 105", catalog);
    execute_query("SELECT dept, COUNT(age), AVG(age) FROM users GROUP BY dept", catalog);
    execute_query("SELECT dept, SUM(age) FROM users WHERE age > 20 GROUP BY dept", catalog);

    return 0;
}