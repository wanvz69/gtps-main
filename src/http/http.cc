#include "http.hh"

#include <cpr/cpr.h>
#include <sandbird.h>
#include <fmt/core.h>

#include <atomic>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <cstring>
#include <cstdlib>

#include "constants/config.hh"
#include "utils/io.hh"

namespace beef::http
{
    namespace
    {

        static std::string base64_encode(const std::string& input)
        {
            static constexpr char table[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz"
                "0123456789+/";

            std::string output;
            output.reserve(((input.size() + 2) / 3) * 4);

            unsigned int val = 0;
            int valb = -6;

            for (unsigned char c : input)
            {
                val = (val << 8) + c;
                valb += 8;

                while (valb >= 0)
                {
                    output.push_back(table[(val >> valb) & 0x3F]);
                    valb -= 6;
                }
            }

            if (valb > -6)
                output.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);

            while (output.size() % 4)
                output.push_back('=');

            return output;
        }

        static int hex_value(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';

            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;

            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;

            return -1;
        }

        static std::string url_decode(const std::string& input)
        {
            std::string output;
            output.reserve(input.size());

            for (size_t i = 0; i < input.size(); ++i)
            {
                if (input[i] == '+')
                {
                    output.push_back(' ');
                }
                else if (input[i] == '%' && i + 2 < input.size())
                {
                    const int hi = hex_value(input[i + 1]);
                    const int lo = hex_value(input[i + 2]);

                    if (hi >= 0 && lo >= 0)
                    {
                        output.push_back(
                            static_cast<char>((hi << 4) | lo)
                        );

                        i += 2;
                    }
                    else
                    {
                        output.push_back(input[i]);
                    }
                }
                else
                {
                    output.push_back(input[i]);
                }
            }

            return output;
        }

        static std::string json_escape(const std::string& input)
        {
            std::string output;
            output.reserve(input.size() + 16);

            for (char c : input)
            {
                switch (c)
                {
                case '"':
                    output += "\\\"";
                    break;

                case '\\':
                    output += "\\\\";
                    break;

                case '\b':
                    output += "\\b";
                    break;

                case '\f':
                    output += "\\f";
                    break;

                case '\n':
                    output += "\\n";
                    break;

                case '\r':
                    output += "\\r";
                    break;

                case '\t':
                    output += "\\t";
                    break;

                default:
                    output.push_back(c);
                    break;
                }
            }

            return output;
        }

        static void send_json(
            sb_Stream* stream,
            int status,
            const std::string& body)
        {
            sb_send_status(stream, status, status == 200 ? "OK" : "Bad Request");
            sb_send_header(stream, "Content-Type", "application/json");
            sb_send_header(stream, "Cache-Control", "no-store");
            sb_send_header(
                stream,
                "Content-Length",
                fmt::format("{}", body.size()).c_str()
            );

            sb_write(stream, body.data(), body.size());
        }

        static std::string get_var(sb_Stream* stream, const char* name)
        {
            char buffer[16384]{};

            if (sb_get_var(stream, name, buffer, sizeof(buffer)) == 0)
                return {};

            return url_decode(buffer);
        }

        static const char* dashboard_template = R"HTML(
<!DOCTYPE html>
<html lang="en" style="background-color:rgba(0,0,0,0.0);width:100%;height:100%;">
<head>
<meta charset="utf-8">
<meta http-equiv="X-UA-Compatible" content="IE=edge">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Growtopia Player Support</title>

<link rel="icon" type="image/png"
href="https://s3.eu-west-1.amazonaws.com/cdn.growtopiagame.com/website/resources/assets/images/growtopia.ico">

<link rel="stylesheet"
href="https://s3.eu-west-1.amazonaws.com/cdn.growtopiagame.com/website/resources/assets/css/faq-main.css">

<link rel="stylesheet"
href="https://s3.eu-west-1.amazonaws.com/cdn.growtopiagame.com/website/resources/assets/css/shop-custom.css">

<link rel="stylesheet"
href="https://s3.eu-west-1.amazonaws.com/cdn.growtopiagame.com/website/resources/assets/css/ingame-custom.css">

<style>
.modal-backdrop {
    background-color: rgba(0,0,0,0.1) !important;
}

.modal-backdrop+div {
    overflow:auto;
}

.modal-body,
.content {
    padding:0;
}

.hidden {
    display:none !important;
}
</style>
</head>

<body style="background-color:rgba(0,0,0,0.0);">

<button
type="button"
class="btn btn-primary hidden"
data-toggle="modal"
id="modalButton"
data-target="#modalShow"
data-backdrop="static"
data-keyboard="false">
</button>

<div class="content">
<section class="common-box">
<div class="container">
<div class="row">
<div class="col-md-12 col-sm-12">

<div class="modal fade product-list-popup"
id="modalShow"
tabindex="-1"
role="dialog"
aria-hidden="false">

<div class="modal-dialog modal-dialog-centered" role="document">
<div class="modal-content">
<div class="modal-body">

<section class="common-box">
<div class="container">

<div class="section-title center-align">
<h2>Log in with your Grow ID</h2>
</div>

<div class="row div-content-center">
<div class="col-md-12 col-sm-12">

<form
method="POST"
id="loginForm"
action="/player/growid/login/validate"
accept-charset="UTF-8"
role="form"
autocomplete="off">

<input
name="_token"
type="hidden"
value="{{ data }}">

<div class="form-group">
<input
id="login-name"
class="form-control grow-text"
required
placeholder="Your Growtopia Name *"
name="growId"
type="text"
pattern="[A-Za-z0-9]+"
title="Only letters and numbers are allowed">
</div>

<div class="form-group">
<input
id="password"
class="form-control grow-text"
required
placeholder="Your Growtopia Password *"
name="password"
type="password"
pattern="[A-Za-z0-9@._!\-]+"
title="Only letters, numbers, and @ . _ ! - are allowed">
</div>

<div class="form-group text-center forgot-password">
<a
href="https://www.growtopiagame.com/account"
target="_blank">
Forgot Password
</a>
<br>

<a
href="#"
id="toggleRegister"
style="margin-top:5px;display:inline-block;">
Register Account
</a>
</div>

<div class="form-group text-center">
<input
class="btn btn-lg btn-primary grow-button"
type="submit"
value="Log in">
</div>

</form>


<form
method="POST"
id="registerForm"
action="/player/growid/login/validate"
accept-charset="UTF-8"
class="hidden"
role="form"
autocomplete="off">

<input
name="_token"
type="hidden"
value="{{ data }}">

<div class="form-group">
<input
id="register-name"
class="form-control grow-text"
required
placeholder="Your Growtopia Name *"
name="growId"
type="text"
pattern="[A-Za-z0-9]+">
</div>

<div class="form-group">
<input
id="register-email"
class="form-control grow-text"
required
placeholder="Your Email *"
name="email"
type="email">
</div>

<div class="form-group">
<input
id="register-password"
class="form-control grow-text"
required
placeholder="Your Growtopia Password *"
name="password"
type="password"
pattern="[A-Za-z0-9@._!\-]+">
</div>

<div class="form-group">
<input
id="register-confirm-password"
class="form-control grow-text"
required
placeholder="Confirm Password *"
name="password_confirmation"
type="password">
</div>

<div class="form-group text-center">
<a
href="#"
id="toggleLogin"
style="margin-bottom:10px;display:inline-block;">
Back to Login
</a>
</div>

<div class="form-group text-center">
<input
class="btn btn-lg btn-primary grow-button"
type="submit"
value="Register">
</div>

</form>

</div>
</div>

</div>
</section>

</div>
</div>
</div>

</div>
</div>

</div>
</div>
</div>
</section>
</div>

<script>
document.addEventListener('DOMContentLoaded', function () {

    const loginForm =
        document.getElementById('loginForm');

    const registerForm =
        document.getElementById('registerForm');

    const toggleRegister =
        document.getElementById('toggleRegister');

    const toggleLogin =
        document.getElementById('toggleLogin');

    const sectionTitle =
        document.querySelector('.section-title h2');

    toggleRegister.addEventListener('click', function (e) {
        e.preventDefault();

        loginForm.classList.add('hidden');
        registerForm.classList.remove('hidden');

        sectionTitle.textContent =
            'Register your Grow ID';
    });

    toggleLogin.addEventListener('click', function (e) {
        e.preventDefault();

        registerForm.classList.add('hidden');
        loginForm.classList.remove('hidden');

        sectionTitle.textContent =
            'Log in with your Grow ID';
    });

    const loginSubmitButton =
        loginForm.querySelector('input[type="submit"]');

    loginForm.addEventListener('submit', function (e) {

        if (loginSubmitButton.disabled) {
            e.preventDefault();
            return false;
        }

        loginSubmitButton.disabled = true;
        loginSubmitButton.value = 'Logging in...';
    });

    const registerSubmitButton =
        registerForm.querySelector('input[type="submit"]');

    registerForm.addEventListener('submit', function (e) {

        const password =
            document.getElementById('register-password').value;

        const confirmation =
            document.getElementById('register-confirm-password').value;

        if (password !== confirmation) {
            e.preventDefault();
            alert('Passwords do not match!');
            return false;
        }

        registerSubmitButton.disabled = true;
        registerSubmitButton.value = 'Registering...';
    });

    document
        .getElementById('login-name')
        .addEventListener('input', function () {
            this.value =
                this.value.replace(/[^A-Za-z0-9]/g, '');
        });

    document
        .getElementById('password')
        .addEventListener('input', function () {
            this.value =
                this.value.replace(/[^A-Za-z0-9@._!\-]/g, '');
        });

});

window.addEventListener('load', function () {

    const button =
        document.getElementById('modalButton');

    if (button) {
        button.click();
    }

});
</script>

<script
src="https://s3.eu-west-1.amazonaws.com/cdn.growtopiagame.com/website/resources/assets/js/jquery-3.7.1.min.js">
</script>

<script
src="https://s3.eu-west-1.amazonaws.com/cdn.growtopiagame.com/website/resources/assets/js/bootstrap3.min.js">
</script>

</body>
</html>
)HTML";

        static std::string make_login_token(
            const std::string& token,
            const std::string& grow_id,
            const std::string& password,
            const std::string& email)
        {
            std::string payload;

            if (!email.empty())
            {
                payload =
                    "_token=" + token +
                    "&growId=" + grow_id +
                    "&password=" + password +
                    "&email=" + email +
                    "&reg=1";
            }
            else
            {
                payload =
                    "_token=" + token +
                    "&growId=" + grow_id +
                    "&password=" + password +
                    "&reg=0";
            }

            return base64_encode(payload);
        }

        static void send_login_success(
            sb_Stream* stream,
            const std::string& token)
        {
            const std::string json =
                "{"
                "\"status\":\"success\","
                "\"message\":\"Account Validated.\","
                "\"token\":\"" + json_escape(token) + "\","
                "\"url\":\"\","
                "\"accountType\":\"growtopia\""
                "}";

            send_json(stream, 200, json);
        }

        static void send_login_error(
            sb_Stream* stream,
            int status,
            const std::string& message)
        {
            const std::string json =
                "{"
                "\"status\":\"error\","
                "\"message\":\"" + json_escape(message) + "\""
                "}";

            send_json(stream, status, json);
        }
    }

    int handler(sb_Event* e)
    {
        if (e == nullptr)
            return SB_RES_CLOSE;

        if (e->type != SB_EV_REQUEST)
            return SB_RES_OK;

        if (e->path == nullptr || e->method == nullptr)
            return SB_RES_CLOSE;

        if (std::strlen(e->path) >= 512)
        {
            sb_send_status(
                e->stream,
                414,
                "Request-URI Too Long");

            return SB_RES_OK;
        }

        fmt::print(
            "{} - {} {}\n",
            e->address ? e->address : "-",
            e->method,
            e->path);

        if (
            std::strcmp(
                e->path,
                "/growtopia/server_data.php") == 0 &&
            std::strcmp(
                e->method,
                "POST") == 0)
        {
            sb_send_status(e->stream, 200, "OK");
            sb_send_header(
                e->stream,
                "Content-Type",
                "text/plain");

            sb_writef(
                e->stream,
                "server|%s\n"
                "port|%d\n"
                "type|1\n"
                "%s\n"
                "beta_server|127.0.0.1\n"
                "beta_port|6969\n"
                "beta_type|1\n"
                "meta|undefined\n"
                "RTENDMARKERBS1001",
                config::address.data(),
                config::enet::server_port,
                "#maint|`5Server is under maintenance.");

            return SB_RES_OK;
        }

        if (
            std::strcmp(
                e->path,
                "/player/login/dashboard") == 0 &&
            (
                std::strcmp(e->method, "POST") == 0 ||
                std::strcmp(e->method, "GET") == 0
                ))
        {
            std::string client_data;

            /*
             * GTLogin receives the raw client payload as a form body.
             *
             * Sandbird's sb_get_var() is used for named variables.
             * Growtopia's dashboard request can arrive as a single
             * form key, so try the common "data" / "clientData"
             * names first.
             */
            client_data = get_var(e->stream, "clientData");

            if (client_data.empty())
                client_data = get_var(e->stream, "data");

            /*
             * If there is no named value, use a harmless empty payload.
             * The endpoint must still return HTML because the client
             * expects a dashboard response.
             */
            const std::string encoded =
                base64_encode(client_data);

            std::string html = dashboard_template;

            const std::string placeholder = "{{ data }}";

            const size_t position =
                html.find(placeholder);

            if (position != std::string::npos)
            {
                html.replace(
                    position,
                    placeholder.size(),
                    encoded);
            }

            sb_send_status(
                e->stream,
                200,
                "OK");

            sb_send_header(
                e->stream,
                "Content-Type",
                "text/html; charset=utf-8");

            sb_send_header(
                e->stream,
                "Cache-Control",
                "no-store");

            sb_send_header(
                e->stream,
                "Content-Length",
                fmt::format(
                    "{}",
                    html.size()).c_str());

            sb_write(
                e->stream,
                html.data(),
                html.size());

            return SB_RES_OK;
        }

        if (
            std::strcmp(
                e->path,
                "/player/growid/login/validate") == 0 &&
            std::strcmp(
                e->method,
                "POST") == 0)
        {
            const std::string token =
                get_var(e->stream, "_token");

            const std::string grow_id =
                get_var(e->stream, "growId");

            const std::string password =
                get_var(e->stream, "password");

            const std::string email =
                get_var(e->stream, "email");

            if (grow_id.empty())
            {
                send_login_error(
                    e->stream,
                    400,
                    "GrowID is required.");

                return SB_RES_OK;
            }

            if (password.empty())
            {
                send_login_error(
                    e->stream,
                    400,
                    "Password is required.");

                return SB_RES_OK;
            }

            /*
             * IMPORTANT:
             *
             * GTLogin's current implementation does not perform a
             * database lookup. It creates the token directly.
             *
             * We intentionally keep this first C++ integration
             * compatible with that protocol.
             *
             * The actual GTPS credential validation still happens
             * inside the normal game login flow.
             */
            const std::string login_token =
                make_login_token(
                    token,
                    grow_id,
                    password,
                    email);

            send_login_success(
                e->stream,
                login_token);

            return SB_RES_OK;
        }

        if (
            std::strcmp(
                e->path,
                "/player/growid/checktoken") == 0)
        {
            sb_send_status(
                e->stream,
                307,
                "Temporary Redirect");

            sb_send_header(
                e->stream,
                "Location",
                "/player/growid/validate/checktoken");

            sb_send_header(
                e->stream,
                "Cache-Control",
                "no-store");

            sb_writef(
                e->stream,
                "Token validation endpoint");

            return SB_RES_OK;
        }

        if (
            std::strcmp(
                e->path,
                "/player/growid/validate/checktoken") == 0)
        {
            std::string refresh_token =
                get_var(
                    e->stream,
                    "refreshToken");

            if (refresh_token.empty())
            {
                refresh_token =
                    get_var(
                        e->stream,
                        "token");
            }

            if (refresh_token.empty())
            {
                send_login_error(
                    e->stream,
                    400,
                    "refreshToken is required.");

                return SB_RES_OK;
            }

            const std::string json =
                "{"
                "\"status\":\"success\","
                "\"message\":\"Token is valid.\","
                "\"token\":\"" +
                json_escape(refresh_token) +
                "\","
                "\"url\":\"\","
                "\"accountType\":\"growtopia\""
                "}";

            send_json(
                e->stream,
                200,
                json);

            return SB_RES_OK;
        }

        if (std::strcmp(e->method, "GET") == 0)
        {
            if (
                std::strstr(e->path, "/game/") != nullptr ||
                std::strstr(e->path, "/social/") != nullptr ||
                std::strstr(e->path, "/interface/") != nullptr ||
                std::strstr(e->path, "/audio/") != nullptr)
            {
                if (std::strlen(e->path) > 512)
                    return SB_RES_CLOSE;

                char path[554] = "data/cache";

#ifdef _WIN32
                strcat_s(
                    path,
                    sizeof(path),
                    e->path);
#else
                std::strncat(
                    path,
                    e->path,
                    sizeof(path) - std::strlen(path) - 1);
#endif

                fmt::print(
                    "Get Path: {}\n",
                    path);

                if (!std::filesystem::exists(path))
                {
                    fmt::print(
                        "Cache missing: {}\n",
                        path);

                    const std::string str_path = path;

                    try
                    {
                        const auto parent =
                            std::filesystem::path(
                                str_path).parent_path();

                        if (!parent.empty())
                        {
                            std::filesystem::create_directories(
                                parent);
                        }

                        cpr::Response r =
                            cpr::Get(
                                cpr::Url{
                                    fmt::format(
                                        "http://{}/{}{}",
                                        config::cdn::ubisoft::server_address,
                                        config::cdn::ubisoft::server_path,
                                        e->path)
                                });

                        if (r.status_code >= 200 &&
                            r.status_code < 300 &&
                            !r.text.empty())
                        {
                            io::write_all_bytes(
                                path,
                                r.text.data(),
                                r.text.size());
                        }
                    }
                    catch (const std::exception& ex)
                    {
                        fmt::print(
                            "Cache download failed: {}\n",
                            ex.what());
                    }
                }

                if (!std::filesystem::exists(path))
                {
                    sb_send_status(
                        e->stream,
                        404,
                        "Not Found");

                    return SB_RES_OK;
                }

                uintmax_t filesize = 0;

                void* file =
                    io::read_all_bytes(
                        path,
                        filesize);

                if (!file)
                {
                    sb_send_status(
                        e->stream,
                        404,
                        "Not Found");

                    return SB_RES_OK;
                }

                sb_send_status(
                    e->stream,
                    200,
                    "OK");

                sb_send_header(
                    e->stream,
                    "Content-Type",
                    "text/plain");

                sb_send_header(
                    e->stream,
                    "Content-Length",
                    fmt::format(
                        "{}",
                        filesize).c_str());

                sb_send_header(
                    e->stream,
                    "Connection",
                    "keep-alive");

                sb_send_header(
                    e->stream,
                    "Accept-Ranges",
                    "bytes");

                sb_write(
                    e->stream,
                    file,
                    filesize);

                std::free(file);

                return SB_RES_OK;
            }
        }

        sb_send_status(
            e->stream,
            404,
            "Not Found");

        sb_send_header(
            e->stream,
            "Content-Type",
            "text/plain");

        sb_writef(
            e->stream,
            "404 - Not Found");

        return SB_RES_OK;
    }

    void serve(std::atomic<bool>& running)
    {
        sb_Options opt{};
        sb_Server* server = nullptr;

        opt.host = "0.0.0.0";
        opt.port = "80";
        opt.handler = handler;

        server = sb_new_server(&opt);

        if (!server)
        {
            fmt::print(
                stderr,
                "failed to initialize http server on port 80\n");

            return;
        }

        fmt::print(
            "HTTP server running at http://0.0.0.0:{}\n",
            opt.port);

        while (running.load())
        {
            sb_poll_server(
                server,
                1000);
        }

        sb_close_server(server);

        fmt::print(
            "HTTP server stopped\n");
    }
}