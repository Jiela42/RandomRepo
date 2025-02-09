#include "MatrixLib/striped_Matrix.hpp"
#include "MatrixLib/sparse_CSR_Matrix.hpp"
#include "UtilLib/cuda_utils.hpp"
#include "MatrixLib/coloring.cuh"

#include <vector>
#include <iostream>
#include <string>

// #include <stdio.h>

template <typename T>
striped_Matrix<T>::striped_Matrix() {
    this->nx = 0;
    this->ny = 0;
    this->nz = 0;
    this->nnz = 0;
    this->diag_index = -1;
    this->matrix_type = MatrixType::UNKNOWN;
    this->num_rows = 0;
    this->num_cols = 0;
    this->num_stripes = 0;
    this->j_min_i.clear();
    this->values.clear();
    this->color_pointer_d = nullptr;
    this->color_sorted_rows_d = nullptr;
}

template <typename T>
striped_Matrix<T>::~striped_Matrix(){
    if (this->color_pointer_d != nullptr) {
        CHECK_CUDA(cudaFree(this->color_pointer_d));
        this->color_pointer_d = nullptr;

    }
    if (this->color_sorted_rows_d != nullptr) {
        CHECK_CUDA(cudaFree(this->color_sorted_rows_d));
        this->color_sorted_rows_d = nullptr;
    }
}

template <typename T>
void striped_Matrix<T>::striped_Matrix_from_sparse_CSR(sparse_CSR_Matrix<T>& A){
    if (A.get_matrix_type() == MatrixType::Stencil_3D27P) {
        this->striped_3D27P_Matrix_from_CSR(A);
    } else {
        printf("ERROR: Unsupported matrix type for conversion to striped matrix\n");
        exit(1);
    }
}

template <typename T>
void striped_Matrix<T>::striped_3D27P_Matrix_from_CSR(sparse_CSR_Matrix<T>& A){
    
    assert(A.get_matrix_type() == MatrixType::Stencil_3D27P);
    this->matrix_type = MatrixType::Stencil_3D27P;
    this->nx = A.get_nx();
    this->ny = A.get_ny();
    this->nz = A.get_nz();
    this->nnz = A.get_nnz();
    this->num_rows = A.get_num_rows();
    this->num_cols = A.get_num_cols();
    this->num_stripes = 27;
    this->j_min_i = std::vector<int>(this->num_stripes, 0);
    this->values = std::vector<T>(this->num_stripes * this->num_rows, 0);
    this->color_pointer_d = nullptr;
    this->color_sorted_rows_d = nullptr;

    // first we make our mapping for the j_min_i
    // each point has num_stripe neighbours and each is associated with a coordinate relative to the point
    // the point itself is a neighobour, too {0,0,0}
    int neighbour_offsets [num_stripes][3] = {
        {-1, -1, -1}, {0, -1, -1}, {1, -1, -1},
        {-1, 0, -1}, {0, 0, -1}, {1, 0, -1},
        {-1, 1, -1}, {0, 1, -1}, {1, 1, -1},
        {-1, -1, 0}, {0, -1, 0}, {1, -1, 0},
        {-1, 0, 0}, {0, 0, 0}, {1, 0, 0},
        {-1, 1, 0}, {0, 1, 0}, {1, 1, 0},
        {-1, -1, 1}, {0, -1, 1}, {1, -1, 1},
        {-1, 0, 1}, {0, 0, 1}, {1, 0, 1},
        {-1, 1, 1}, {0, 1, 1}, {1, 1, 1}
    };

    for (int i = 0; i < this->num_stripes; i++) {

        int off_x = neighbour_offsets[i][0];
        int off_y = neighbour_offsets[i][1];
        int off_z = neighbour_offsets[i][2];
        
        this->j_min_i[i] = off_x + off_y * this->nx + off_z * this->nx * this->ny;
        if (this->j_min_i[i] == 0) {
            this->diag_index = i;
        }
    }

    int elem_ctr = 0;

    // now that we have the static offsets which define i & j, we can make the actual matrix
    for (int i = 0; i < this->num_rows; i++) {
        for (int stripe_j = 0; stripe_j < this->num_stripes; stripe_j++) {
            int j = this->j_min_i[stripe_j] + i;
            // check if j is in bounds (since not every point has all 27 neighbours)
            if (j >= 0 && j < this->num_cols) {
                double elem = A.get_element(i, j);
                // also make sure we don't add zero elements
                if(elem != 0){
                    this->values[i * this->num_stripes + stripe_j] = elem;
                    elem_ctr++;
                }
            }
        }
    }
    assert(elem_ctr == this->nnz);
}

template <typename T>
void striped_Matrix<T>::generate_coloring(){

    int num_colors = (nx -1) + 2 * (ny-1) + 4 * (nz-1) + 1;

    // first we allocate the space on the GPU
    CHECK_CUDA(cudaMalloc(&this->color_pointer_d, (num_colors + 1) * sizeof(int)));
    CHECK_CUDA(cudaMalloc(&this->color_sorted_rows_d, this->num_rows * sizeof(int)));

    get_color_row_mapping(this->nx, this->ny, this->nz, this->color_pointer_d, this->color_sorted_rows_d);
}

template <typename T>
int* striped_Matrix<T>::get_color_pointer_d(){
    return this->color_pointer_d;
}

template <typename T>
int* striped_Matrix<T>::get_color_sorted_rows_d(){
    return this->color_sorted_rows_d;
}

template <typename T>
std::vector<int> striped_Matrix<T>::get_color_pointer_vector(){

    int num_colors = (this->nx-1) + 2 * (this->ny -1) + 4 *(this->nz-1) + 1;
    std::vector<int> color_pointer(num_colors + 1, 0);
    CHECK_CUDA(cudaMemcpy(color_pointer.data(), this->color_pointer_d, (num_colors + 1) * sizeof(int), cudaMemcpyDeviceToHost));
    return color_pointer;
}

template <typename T>
std::vector<int> striped_Matrix<T>::get_color_sorted_rows_vector(){

    std::vector<int> color_sorted_rows(this->num_rows, 0);
    CHECK_CUDA(cudaMemcpy(color_sorted_rows.data(), this->color_sorted_rows_d, this->num_rows * sizeof(int), cudaMemcpyDeviceToHost));
    return color_sorted_rows;
}

template <typename T>
void striped_Matrix<T>::print_COR_Format(){
    int max_color = (this->nx-1) + 2 * (this->ny-1) + 4 * (this->nz -1) + 1;
    ::print_COR_Format(max_color, this->num_rows, this->color_pointer_d, this->color_sorted_rows_d);
}

template <typename T>
int striped_Matrix<T>::get_num_rows() const{
    return this->num_rows;
}

template <typename T>
int striped_Matrix<T>::get_num_cols() const{
    return this->num_cols;
}

template <typename T>
int striped_Matrix<T>::get_num_stripes() const{
    return this->num_stripes;
}

template <typename T>
int striped_Matrix<T>::get_nx() const{
    return this->nx;
}

template <typename T>
int striped_Matrix<T>::get_ny() const{
    return this->ny;
}

template <typename T>
int striped_Matrix<T>::get_nz() const{
    return this->nz;
}


template <typename T>
int striped_Matrix<T>::get_nnz() const{
    return this->nnz;
}

template <typename T>
int striped_Matrix<T>::get_diag_index() const{
    return this->diag_index;
}

template <typename T>
MatrixType striped_Matrix<T>::get_matrix_type() const{
    return this->matrix_type;
}

template <typename T>
std::vector<int>& striped_Matrix<T>::get_j_min_i(){
    return this->j_min_i;
}

template <typename T>
std::vector<T>& striped_Matrix<T>::get_values(){
    return this->values;
}

template <typename T>
T striped_Matrix<T>::get_element(int i, int j) const{

    // check if j is in bounds (since not every point has all 27 neighbours)
    if (j >= 0 && j < this->num_cols) {
        for (int stripe_j = 0; stripe_j < this->num_stripes; stripe_j++) {
            if (j == i + this->j_min_i[stripe_j]) {
                return this->values[i * this->num_stripes + stripe_j];
            }
        }
    }
    // printf("WARNING Element row %d, col %d not found\n", i, j);
    return T();
}

template <typename T>
void striped_Matrix<T>::set_num_rows(int num_rows){
    this->num_rows = num_rows;
}

template <typename T>
void striped_Matrix<T>::print() const{
    std::cout << "striped Matrix: " << std::endl;
    std::cout << "nx: " << this->nx << " ny: " << this->ny << " nz: " << this->nz << std::endl;
    std::cout << "num_rows: " << this->num_rows << " num_cols: " << this->num_cols << std::endl;
    std::cout << "num_stripes: " << this->num_stripes << std::endl;
    std::cout << "j_min_i: ";
    for (int i = 0; i < this->num_stripes; i++) {
        std::cout << this->j_min_i[i] << " ";
    }
    std::cout << std::endl;
    std::cout << "Values: ";
    for (int i = 0; i < this->num_rows; i++) {
        for (int j = 0; j < this->num_stripes; j++) {
            std::cout << this->values[i * this->num_stripes + j] << " ";
        }
        std::cout << std::endl;
    }
}

// explicit template instantiation
template class striped_Matrix<double>;