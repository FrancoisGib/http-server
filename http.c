#include "http.h"

int sock = -1;
int nb_processes = NB_PROCESSES;
tree_t *http_tree;
response_t error_response;
pid_t parent_pid;

void sigint_handler(int code)
{
   printf("Process %d ended\n", getpid());
   if (sock != -1)
   {
      close(sock);
   }
   if (http_tree != NULL)
   {
      free_http_tree(http_tree);
   }
   if (getpid() == parent_pid)
   {
      printf("\nClosed socket");
      printf("\nFreed http tree\n");
   }
   exit(0);
}

int read_file(char buffer[1024], char *file_path)
{
   stat_t st;
   stat(file_path, &st);
   if (!S_ISDIR(st.st_mode) && access(file_path, R_OK) == 0)
   {
      FILE *file = fopen(file_path, "r");
      return (int)fread(buffer, 1, 1024, file);
      fclose(file);
   }
   return -1;
}

void construct_response(int client_socket, http_request_t *http_request)
{
   endpoint_t *endpoint = get_endpoint(http_tree, http_request->path);
   if (endpoint == NULL)
   {
      printf("No endpoint for path: %s \n", http_request->path);
      return;
   }

   int content_length = 0;
   char buffer[1024];
   memset(buffer, 0, 1024);
   if (endpoint->response.type == ET_FILE)
   {
      content_length = read_file(buffer, endpoint->response.resource.content);
   }
   else if (endpoint->response.type == ET_TEXT)
   {
      strcpy(buffer, endpoint->response.resource.content);
      content_length = strlen(endpoint->response.resource.content);
   }
   else if (endpoint->response.type == ET_FUNC)
   {
      resource_function function = endpoint->response.resource.function;
      char *response_content = function(http_request->body);
      content_length = (int)strlen(response_content);
      strcpy(buffer, response_content);
      free(response_content);
   }
   else if (endpoint->response.type == ET_DIRECTORY)
   {
      while (strncmp(http_request->path, endpoint->path, strlen(endpoint->path)) != 0)
      {
         http_request->path++;
      }
      http_request->path += strlen(endpoint->path) + 1; /* pass the \0 made by get_endpoint */
      char file_path[strlen(http_request->path) + strlen(endpoint->response.resource.content) + 1];
      strcpy(file_path, endpoint->response.resource.content);
      strcat(file_path, http_request->path);
      content_length = read_file(buffer, file_path);
   }

   char content_type_str[64] = "Content-Type: ";
   if (content_length == -1)
   {
      char message[] = "Error";
      strcpy(buffer, message);
      content_length = strlen(message);
      strcat(content_type_str, get_content_type(error_response.content_type));
   }
   else
   {
      if (endpoint->response.content_type == NULL_CONTENT)
      {
         strcat(content_type_str, get_content_type_with_file_extension(http_request->path));
      }
      else
      {
         strcat(content_type_str, get_content_type(endpoint->response.content_type));
      }
   }
   strcat(content_type_str, "\r\n\r\n");

   char content_length_buffer[5];
   sprintf(content_length_buffer, "%d", content_length);

   char content_length_str[32] = "Content-Length: ";
   strcat(content_length_str, content_length_buffer);
   strcat(content_length_str, "\r\n");

   char response[MAX_RESPONSE_SIZE] = "HTTP/1.1 ";

   char status_code[6];
   sprintf(status_code, "%d\r\n", endpoint->response.status);
   strcat(response, status_code);

   strcat(response, content_length_str);
   strcat(response, content_type_str);

   strcat(response, buffer);

   printf("-----------------\n%s\n-----------------\n", response);

   int write_size = (int)write(client_socket, response, strlen(response));
   if (write_size < (int)strlen(response))
   {
      printf("Error while sending response\n");
      write_log("Error while sending response\n");
   }
}

void send_error_response(int client_socket)
{
   printf("Error response sent\n");
}

int http_request_parse_request_line(http_request_t *http_request, char **request_ptr)
{
   char *request = *request_ptr;
   http_request->method = strtok_r(request, " ", &request);
   if (http_request->method == NULL)
   {
      printf("Invalid request received");
      return -1;
   }
   else
   {
      http_request->method = strdup(http_request->method);
   }

   http_request->path = strtok_r(request, " ", &request);
   if (http_request->path == NULL)
   {
      printf("Invalid request received\n");
      return -1;
   }
   else
   {
      http_request->path = strdup(http_request->path);
   }

   http_request->http_version = strtok_r(request, "\n", &request);
   if (http_request->http_version == NULL)
   {
      printf("Invalid request received\n");
      return -1;
   }
   else
   {
      http_request->http_version = strdup(http_request->http_version);
   }
   *request_ptr = request;
   return 0;
}

int http_request_parse_headers(http_request_t *http_request, char **request_ptr)
{
   char *request = *request_ptr;
   int request_size = strlen(request);

   if (request_size == 0)
   {
      printf("Empty request\n");
      return 1;
   }

   char *header_end = strstr(request, "\r\n\r\n");
   if (!header_end)
   {
      printf("Invalid request\n");
      return -1;
   }

   *header_end = '\0';
   char *body = header_end + 4; // Body pointer after "\r\n\r\n"

   char *header_str;
   while ((header_str = strtok_r(request, "\r\n", &request)) != NULL)
   {
      header_t *header = malloc(sizeof(header_t));
      char *header_name = strtok_r(header_str, ":", &header_str);
      if (header_name && *header_str == ' ')
      {
         header_str++;
      }
      header->name = strdup(header_name);
      header->value = strdup(header_str);
      if (strcmp(header_name, "Content-Length") == 0)
      {
         http_request->content_length = (int)strtol(header->value, NULL, 10);
      }
      http_request->headers = insert_in_head(http_request->headers, header);
      http_request->headers_length += strlen(header->name) + strlen(header->value) + 4; // +1 to add \n\t and ": " later
   }
   *request_ptr = body;
   return *body != '\0'; // 1 if this is not the end of the headers part, 0 if this is the body or empty.
}

int http_request_parse_body(http_request_t *http_request, char **request_ptr)
{
   char *request = *request_ptr;
   int request_size = strlen(request);
   int added_body_length = MIN(request_size, http_request->content_length);
   if (http_request->body == NULL)
   {
      http_request->body = malloc(http_request->content_length + 1);
      strncpy(http_request->body, request, added_body_length);
      http_request->body[added_body_length] = '\0';
   }
   else
   {
      int body_size = strlen(http_request->body);
      int remaining_body_size = http_request->content_length - body_size;
      strncat(&http_request->body[body_size], request, remaining_body_size);
   }
   return added_body_length == http_request->content_length;
}

void *http_request_write_log_wrapper(void *http_request)
{
   http_request_write_log((http_request_t *)http_request);
   return NULL;
}

void accept_connection(void)
{
   sockaddr_in_t client_address;
   unsigned int client_address_len = sizeof(client_address);
   int client_socket;

   while (1)
   {
      client_socket = accept(sock, (struct sockaddr *)&client_address, &client_address_len);
      if (client_socket == -1)
      {
         perror("Error accepting connection");
         exit(EXIT_FAILURE);
      }

      char buffer[MAX_REQUEST_SIZE];
      http_request_t http_request;
      int is_first_request = 1;
      int is_incorrect_request = 0;
      int header_parsed = 0;
      int done = 0;
      int size;
      while (!done && !is_incorrect_request && (size = read(client_socket, buffer, MAX_REQUEST_SIZE)) > 0)
      {
         char *buffer_ptr = buffer;
         http_request.body = NULL;
         http_request.headers = NULL;
         http_request.content_length = 0;
         http_request.headers_length = 0;
         char **request_ptr = &buffer_ptr;

         if (is_first_request == 1)
         {
            if (http_request_parse_request_line(&http_request, request_ptr) == -1)
            {
               is_incorrect_request = 1;
               continue;
            }
            is_first_request = 0;
         }

         if (!header_parsed && http_request_parse_headers(&http_request, request_ptr) == 1)
         {
            header_parsed = 1;
         }

         if (http_request.content_length > 0)
         {
            done = http_request_parse_body(&http_request, request_ptr);
         }
         else
         {
            done = 1;
         }
         memset(buffer, 0, size);
      }
      http_request_write_log(&http_request);
      // pthread_t log_thread;
      // pthread_create(&log_thread, NULL, http_request_write_log_wrapper, (void *)&http_request);
      // pthread_detach(log_thread);
      construct_response(client_socket, &http_request);
      close(client_socket);
   }
}

void start_server(int port)
{
   signal(SIGINT, sigint_handler);

   if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
   {
      perror("Error creating socket");
      exit(EXIT_FAILURE);
   }

   int opt = 1;
   if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
   {
      perror("Error setting socket options");
      close(sock);
      exit(EXIT_FAILURE);
   }

   struct sockaddr_in sin;
   sin.sin_family = AF_INET;
   sin.sin_port = htons(port);
   sin.sin_addr.s_addr = INADDR_ANY;

   if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) != 0)
   {
      perror("Error binding socket");
      close(sock);
      exit(EXIT_FAILURE);
   }

   if (listen(sock, nb_processes) != 0)
   {
      perror("Error listening on socket");
      exit(EXIT_FAILURE);
   }

   parent_pid = getpid();
   for (int i = 0; i < nb_processes; i++)
   {
      pid_t pid = fork();
      if (pid == 0)
      {
         printf("Worker process %d listening for requests\n", getpid());
         accept_connection();
         exit(0);
      }
      else if (pid < 0)
      {
         perror("Error forking process");
         exit(EXIT_FAILURE);
      }
   }

   while (wait(NULL) > 0)
      ; /* Wait for all child processes to finish */
}
